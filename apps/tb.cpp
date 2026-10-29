// tb —— mini-toolbridge 命令行工具
//
// 用法：
//   tb library                        打印组件库（含给模型看的 prompt）
//   tb render <instructions.json>     校验 + 执行 + 生成 SVG
//       [-o out.svg]
//   tb ask "<自然语言>"               走 LLM 生成指令再执行
//       [-o out.svg] [--offline]       --offline 用固定脚本，不联网
//   tb validate <instructions.json>   只校验，不产出

#include "tb/toolbridge.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace tb;

namespace {

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

// 离线用的固定回复：一段画钟表的指令数组
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
        "  tb library                       打印组件库\n"
        "  tb render <file.json> [-o out]   校验并渲染指令文件\n"
        "  tb validate <file.json>          只校验\n"
        "  tb ask \"<自然语言>\" [-o out]     走大模型（--offline 可离线）\n\n"
        "环境变量（ask 联网时需要）：\n"
        "  TB_API_BASE  例 https://api.deepseek.com/v1\n"
        "  TB_API_KEY   密钥\n"
        "  TB_MODEL     例 deepseek-chat\n");
}

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

    if (cmd == "library") {
        std::printf("%s", render_library_prompt(lib, composites).c_str());
        return 0;
    }

    // 解析公共选项
    std::string out_path = "out.svg";
    bool offline = false;
    std::string positional;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--offline") {
            offline = true;
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

        std::unique_ptr<ILlmProvider> provider;
        if (offline) {
            provider = make_scripted_provider(offline_reply());
            std::printf("[离线模式] 使用固定指令脚本\n");
        } else {
            LlmConfig cfg = llm_config_from_env();
            if (cfg.api_base.empty() || cfg.api_key.empty() || cfg.model.empty()) {
                std::fprintf(stderr,
                             "缺少环境变量。设置 TB_API_BASE / TB_API_KEY / TB_MODEL，"
                             "或加 --offline 走离线脚本。\n");
                return 1;
            }
            std::printf("[在线] %s  model=%s\n", cfg.api_base.c_str(), cfg.model.c_str());
            provider = make_curl_provider(cfg);
        }

        const std::string system_prompt = render_library_prompt(lib, composites);
        std::printf("请求模型…\n");
        const LlmReply reply = provider->complete(system_prompt, positional);
        if (!reply.ok) {
            std::fprintf(stderr, "模型调用失败: %s\n", reply.error.c_str());
            return 1;
        }

        const auto items = extract_instruction_array(reply.text);
        if (items.empty()) {
            std::fprintf(stderr, "没能从回复里解析出指令数组。原始回复：\n%s\n",
                         reply.text.substr(0, 800).c_str());
            return 1;
        }
        std::printf("模型返回 %zu 条指令\n", items.size());

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
