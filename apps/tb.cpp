// tb —— mini-toolbridge 命令行工具
//
//   tb library                       打印组件库（也是喂给模型的 system prompt）
//   tb validate <file.json>          只校验
//   tb render <file.json> [-o out]   校验 + 执行 + 生成 SVG
//   tb ask "<自然语言>" [-o out]      走大模型（同步版）
//   tb ask --stream --url URL        **流式**：边收边校验边执行
//   tb mock-llm [--port N]           本地模拟 SSE 服务，无需 API key

#include "tb/mcp.hpp"
#include "tb/net.hpp"
#include "tb/stream.hpp"
#include "tb/toolbridge.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace tb;

namespace {

namespace asio = boost::asio;

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream ifs(path);
    if (!ifs) {
        ok = false;
        return {};
    }
    std::ostringstream os;
    os << ifs.rdbuf();
    ok = true;
    return os.str();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs) {
        return false;
    }
    ofs << content;
    return static_cast<bool>(ofs);
}

std::string offline_reply() {
    return R"([
{"op":"create_canvas","args":{"width":400,"height":400,"bg":"#f7f7fa"}},
{"op":"create_circle","args":{"cx":200,"cy":200,"r":170,"stroke":"#1f3b66","fill":"#ffffff"}},
{"op":"create_hand","args":{"angle":305,"length":95,"width":9,"stroke":"#1f3b66"}},
{"op":"create_hand","args":{"angle":60,"length":140,"width":6,"stroke":"#c0392b"}},
{"op":"create_circle","args":{"cx":200,"cy":200,"r":6,"stroke":"#1f3b66","fill":"#1f3b66"}}
])";
}

void usage() {
    std::printf(
        "mini-toolbridge —— 自然语言 → 受约束指令 → 校验 → 建模产物\n\n"
        "用法：\n"
        "  tb library                        打印组件库\n"
        "  tb validate <file.json>           只校验\n"
        "  tb render <file.json> [-o out]    校验并渲染\n"
        "  tb ask \"<需求>\" [-o out]          走大模型（同步）\n"
        "  tb ask --stream --url <URL>       流式：边收边校验边执行\n"
        "  tb mock-llm [--port N]            本地 mock SSE 服务（默认 8081）\n"
        "  tb mcp                            MCP stdio 服务端（给 Claude Desktop 等用）\n\n"
        "环境变量（在线时）：TB_API_BASE / TB_API_KEY / TB_MODEL\n");
}

// 从 OpenAI 兼容的 SSE delta 里取 content
std::string delta_content(const std::string& payload) {
    boost::json::error_code ec;
    const boost::json::value v = boost::json::parse(payload, ec);
    if (ec || !v.is_object()) {
        return {};
    }
    const auto& obj = v.as_object();
    if (!obj.contains("choices") || !obj.at("choices").is_array() ||
        obj.at("choices").as_array().empty()) {
        return {};
    }
    const auto& c0 = obj.at("choices").as_array().front();
    if (!c0.is_object() || !c0.as_object().contains("delta")) {
        return {};
    }
    const auto& d = c0.as_object().at("delta");
    if (!d.is_object() || !d.as_object().contains("content")) {
        return {};
    }
    const auto& content = d.as_object().at("content");
    if (!content.is_string()) {
        return {};
    }
    return std::string{content.as_string().data(), content.as_string().size()};
}

struct AbortStream {};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string cmd = argv[1];

    ComponentLibrary lib = make_display_library();
    CompositeRegistry composites;
    composites.add(make_gear_component());

    if (cmd == "mcp") {
        run_mcp_stdio();
        return 0;
    }

    if (cmd == "library") {
        std::printf("%s", render_library_prompt(lib, composites).c_str());
        return 0;
    }

    if (cmd == "mock-llm") {
        std::uint16_t port = 8081;
        bool bad = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--bad") == 0) {
                bad = true;
            }
        }
        run_mock_llm(port, bad);
        return 0;
    }

    // ---- 公共选项 ----
    std::string out_path = "out.svg";
    std::string url;
    bool offline = false;
    bool stream = false;
    std::string positional;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--offline") {
            offline = true;
        } else if (a == "--stream") {
            stream = true;
        } else if (a == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (positional.empty()) {
            positional = a;
        }
    }

    if (cmd == "validate" || cmd == "render") {
        if (positional.empty()) {
            std::fprintf(stderr, "缺少指令文件路径\n");
            return 1;
        }
        bool ok = false;
        const std::string text = read_file(positional, ok);
        if (!ok) {
            std::fprintf(stderr, "读不到文件: %s\n", positional.c_str());
            return 1;
        }
        ModelContext ctx;
        SvgExecutor svg;
        const PipelineStats stats = run_instructions(text, lib, composites, ctx, svg);
        std::printf("通过: %d 条　被拒: %d 条\n", stats.applied, stats.rejected);
        for (const auto& e : stats.errors) {
            std::printf("  ✗ %s\n", e.c_str());
        }
        if (cmd == "validate") {
            return stats.rejected == 0 ? 0 : 2;
        }
        if (!write_file(out_path, svg.document())) {
            std::fprintf(stderr, "写文件失败: %s\n", out_path.c_str());
            return 1;
        }
        std::printf("产物: %s（%zu 个元素）\n", out_path.c_str(), svg.element_count());
        return stats.rejected == 0 ? 0 : 2;
    }

    if (cmd == "ask") {
        if (positional.empty()) {
            std::fprintf(stderr, "缺少需求描述\n");
            return 1;
        }
        if (offline) {
            stream = false;
        }

        // ================= 流式分支 =================
        if (stream) {
            std::string base = url;
            if (base.empty()) {
                const char* env = std::getenv("TB_API_BASE");
                base = env ? env : "";
            }
            if (base.empty()) {
                std::fprintf(stderr,
                             "流式需要 --url 或 TB_API_BASE，例如 "
                             "http://127.0.0.1:8081/v1/chat/completions\n"
                             "可先用 `tb mock-llm` 起一个本地 mock。\n");
                return 1;
            }
            const char* key_env = std::getenv("TB_API_KEY");
            const std::string api_key = key_env ? key_env : "";
            const char* model_env = std::getenv("TB_MODEL");
            const std::string model = model_env ? model_env : "mock-model";

            // 请求体：stream=true，OpenAI 兼容
            boost::json::object body;
            body["model"] = model;
            body["stream"] = true;
            boost::json::array messages;
            messages.push_back(boost::json::object{{"role", "system"},
                                                   {"content", render_library_prompt(lib, composites)}});
            messages.push_back(boost::json::object{{"role", "user"}, {"content", positional}});
            body["messages"] = std::move(messages);
            const std::string payload = boost::json::serialize(body);

            std::vector<std::pair<std::string, std::string>> headers{
                {"content-type", "application/json"}};
            if (!api_key.empty()) {
                headers.emplace_back("authorization", "Bearer " + api_key);
            }

            SseParser sse;
            InstructionScanner scanner;
            ModelContext ctx;
            SvgExecutor svg;

            const auto t0 = std::chrono::steady_clock::now();
            double first_instruction_ms = -1.0;
            int extracted = 0;
            int applied = 0;
            int rejected = 0;
            std::vector<std::string> errors;

            auto elapsed_ms = [&]() {
                return std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0)
                    .count();
            };

            // 每抠出一条完整指令，立刻校验并执行
            auto handle_instruction = [&](const std::string& ins_text) {
                ++extracted;
                if (first_instruction_ms < 0) {
                    first_instruction_ms = elapsed_ms();
                }
                auto parsed = parse(ins_text);
                if (auto* e = std::get_if<ValidationError>(&parsed)) {
                    errors.push_back(e->describe());
                    ++rejected;
                    throw AbortStream{};
                }
                auto result = validate(std::get<boost::json::value>(parsed), lib);
                if (auto* e = std::get_if<ValidationError>(&result)) {
                    errors.push_back(e->describe());
                    ++rejected;
                    throw AbortStream{};
                }
                const Instruction& ins = std::get<Instruction>(result);
                if (auto e = check_preconditions(ins, lib, ctx)) {
                    errors.push_back(e->describe());
                    ++rejected;
                    throw AbortStream{};
                }
                if (!svg.execute(ins)) {
                    errors.push_back("[执行] 不认识的指令 " + ins.op);
                    ++rejected;
                    throw AbortStream{};
                }
                if (ins.op == "create_canvas") {
                    ctx.add_kind("canvas");
                }
                if (!ins.id.empty()) {
                    ctx.add_object(ins.id);
                }
                ++applied;
                std::printf("  + [%6.1f ms] %s\n", elapsed_ms(), ins.op.c_str());
                std::fflush(stdout);
            };

            auto on_chunk = [&](std::string_view bytes) {
                for (const auto& ev : sse.feed(bytes)) {
                    if (ev == "[DONE]") {
                        continue;
                    }
                    const std::string delta = delta_content(ev);
                    if (delta.empty()) {
                        continue;
                    }
                    for (const auto& ins_text : scanner.feed(delta)) {
                        handle_instruction(ins_text);
                    }
                }
            };

            std::printf("流式请求: %s\n", base.c_str());
            asio::io_context ioc{1};
            HttpStreamResult res;
            bool aborted = false;

            asio::co_spawn(
                ioc,
                [&]() -> asio::awaitable<void> {
                    try {
                        res = co_await http_post_stream(ioc, base, headers, payload, on_chunk);
                    } catch (const AbortStream&) {
                        aborted = true;
                    }
                    co_return;
                }(),
                asio::detached);
            ioc.run();

            // 流结束后处理残留
            if (!aborted) {
                for (const auto& ev : sse.flush()) {
                    const std::string delta = delta_content(ev);
                    if (delta.empty()) {
                        continue;
                    }
                    for (const auto& ins_text : scanner.feed(delta)) {
                        try {
                            handle_instruction(ins_text);
                        } catch (const AbortStream&) {
                            aborted = true;
                        }
                    }
                }
            }

            const double total_ms = elapsed_ms();
            if (aborted) {
                std::printf("\n已提前中止（主动断开连接，未继续消费流）\n");
            } else {
                std::printf("\n收到字节: %zu　状态: %d\n", res.bytes, res.status);
                if (!res.error.empty()) {
                    std::printf("网络错误: %s\n", res.error.c_str());
                }
            }
            std::printf("首条指令可用: %.1f ms\n", first_instruction_ms);
            std::printf("全部完成:     %.1f ms\n", total_ms);
            std::printf("提取 %d 条，执行 %d 条，被拒 %d 条\n", extracted, applied, rejected);
            for (const auto& e : errors) {
                std::printf("  ✗ %s\n", e.c_str());
            }
            if (!write_file(out_path, svg.document())) {
                std::fprintf(stderr, "写文件失败: %s\n", out_path.c_str());
                return 1;
            }
            std::printf("产物: %s（%zu 个元素）\n", out_path.c_str(), svg.element_count());
            return rejected == 0 ? 0 : 2;
        }

        // ================= 同步分支 =================
        std::unique_ptr<ILlmProvider> provider;
        if (offline) {
            provider = make_scripted_provider(offline_reply());
            std::printf("[离线模式] 使用固定指令脚本\n");
        } else {
            LlmConfig cfg = llm_config_from_env();
            if (cfg.api_base.empty() || cfg.api_key.empty() || cfg.model.empty()) {
                std::fprintf(stderr,
                             "缺少环境变量 TB_API_BASE / TB_API_KEY / TB_MODEL，"
                             "或加 --offline，或用 --stream --url 走流式。\n");
                return 1;
            }
            std::printf("[在线] %s  model=%s\n", cfg.api_base.c_str(), cfg.model.c_str());
            provider = make_http_provider(cfg);
        }

        const auto t0 = std::chrono::steady_clock::now();
        const LlmReply reply = provider->complete(render_library_prompt(lib, composites), positional);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (!reply.ok) {
            std::fprintf(stderr, "模型调用失败: %s\n", reply.error.c_str());
            return 1;
        }
        const auto items = extract_instruction_array(reply.text);
        if (items.empty()) {
            std::fprintf(stderr, "没能解析出指令数组。原始回复：\n%s\n",
                         reply.text.substr(0, 800).c_str());
            return 1;
        }
        std::printf("模型返回 %zu 条指令（耗时 %.0f ms）\n", items.size(), ms);

        std::string arr = "[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                arr += ",";
            }
            arr += items[i];
        }
        arr += "]";

        ModelContext ctx;
        SvgExecutor svg;
        const PipelineStats stats = run_instructions(arr, lib, composites, ctx, svg);
        std::printf("通过: %d 条　被拒: %d 条\n", stats.applied, stats.rejected);
        for (const auto& e : stats.errors) {
            std::printf("  ✗ %s\n", e.c_str());
        }
        if (!write_file(out_path, svg.document())) {
            std::fprintf(stderr, "写文件失败: %s\n", out_path.c_str());
            return 1;
        }
        std::printf("产物: %s（%zu 个元素）\n", out_path.c_str(), svg.element_count());
        return stats.rejected == 0 ? 0 : 2;
    }

    usage();
    return 1;
}
