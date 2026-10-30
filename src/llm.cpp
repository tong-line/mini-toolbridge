#include "tb/net.hpp"
#include "tb/toolbridge.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tb {
namespace {

// 把参数 schema 渲染成给模型看的说明（基础指令与组合件共用）
void append_args(std::ostringstream& os, const std::vector<Argument>& args) {
    bool first = true;
    for (const auto& a : args) {
        if (!first) {
            os << ", ";
        }
        first = false;
        os << a.name << "(";
        switch (a.type) {
            case ArgType::Number: os << "number"; break;
            case ArgType::String: os << "string"; break;
            case ArgType::Bool:   os << "bool"; break;
            case ArgType::Ref:    os << "ref:" << a.ref_kind; break;
        }
        if (a.has_range) {
            os << ", " << a.min << "~" << a.max;
        }
        os << (!a.required ? ", 可选" : "");
        os << ")";
    }
}

}  // namespace

// ============================================================ 离线 provider

class ScriptedProvider : public ILlmProvider {
public:
    explicit ScriptedProvider(std::string fixed) : fixed_(std::move(fixed)) {}

    LlmReply complete(const std::string&, const std::string&) override {
        return LlmReply{true, fixed_, ""};
    }

private:
    std::string fixed_;
};

std::unique_ptr<ILlmProvider> make_scripted_provider(std::string fixed_reply) {
    return std::make_unique<ScriptedProvider>(std::move(fixed_reply));
}

// ============================================================ 在线 provider
//
// 用 boost::beast 直接调任意 OpenAI 兼容端点，与流式链路共用同一个网络层，带超时。
// 密钥只出现在请求头里，不进命令行参数、不进日志。
//
// 环境变量：
//   TB_API_BASE  例 https://api.deepseek.com/v1
//   TB_API_KEY   密钥
//   TB_MODEL     例 deepseek-chat

LlmReply http_complete(const LlmConfig& cfg, const std::string& system_prompt,
                       const std::string& user_prompt) {
    if (cfg.api_base.empty() || cfg.api_key.empty() || cfg.model.empty()) {
        return LlmReply{false, "",
                        "缺少配置：需要 TB_API_BASE / TB_API_KEY / TB_MODEL"};
    }

    boost::json::object body;
    body["model"] = cfg.model;
    body["temperature"] = 0;
    boost::json::array messages;
    messages.push_back(boost::json::object{{"role", "system"}, {"content", system_prompt}});
    messages.push_back(boost::json::object{{"role", "user"}, {"content", user_prompt}});
    body["messages"] = std::move(messages);
    const std::string payload = boost::json::serialize(body);

    std::string url = cfg.api_base;
    if (url.back() == '/') {
        url.pop_back();
    }
    url += "/chat/completions";

    std::vector<std::pair<std::string, std::string>> headers{
        {"content-type", "application/json"},
        {"authorization", "Bearer " + cfg.api_key},
    };

    std::string response;
    HttpStreamResult res;
    boost::asio::io_context ioc{1};
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void> {
            res = co_await http_post_stream(
                ioc, url, headers, payload,
                [&](std::string_view chunk) { response.append(chunk.data(), chunk.size()); },
                std::chrono::seconds(cfg.timeout_sec));
        }(),
        boost::asio::detached);
    ioc.run();

    if (!res.ok) {
        if (res.timed_out) {
            return LlmReply{false, "",
                            "请求超时（" + std::to_string(cfg.timeout_sec) + " 秒）"};
        }
        return LlmReply{false, "",
                        res.error.empty() ? "HTTP " + std::to_string(res.status) : res.error};
    }
    if (response.empty()) {
        return LlmReply{false, "", "空响应（检查网络与端点）"};
    }

    boost::json::error_code ec;
    const boost::json::value v = boost::json::parse(response, ec);
    if (ec) {
        return LlmReply{false, "", "响应不是合法 JSON: " + response.substr(0, 300)};
    }
    if (!v.is_object()) {
        return LlmReply{false, "", "响应顶层不是 object"};
    }
    const auto& obj = v.as_object();
    if (obj.contains("error")) {
        return LlmReply{false, "", "接口返回错误: " + boost::json::serialize(obj.at("error"))};
    }
    if (!obj.contains("choices") || !obj.at("choices").is_array() ||
        obj.at("choices").as_array().empty()) {
        return LlmReply{false, "", "响应里没有 choices: " + response.substr(0, 300)};
    }
    const auto& choice0 = obj.at("choices").as_array().front();
    if (!choice0.is_object() || !choice0.as_object().contains("message")) {
        return LlmReply{false, "", "choices[0].message 缺失"};
    }
    const auto& msg = choice0.as_object().at("message").as_object();
    if (!msg.contains("content") || !msg.at("content").is_string()) {
        return LlmReply{false, "", "message.content 缺失"};
    }
    return LlmReply{true, std::string{msg.at("content").as_string().data(),
                                      msg.at("content").as_string().size()},
                    ""};
}

class RealHttpProvider : public ILlmProvider {
public:
    explicit RealHttpProvider(LlmConfig cfg) : cfg_(std::move(cfg)) {}

    LlmReply complete(const std::string& system_prompt, const std::string& user_prompt) override {
        return http_complete(cfg_, system_prompt, user_prompt);
    }

private:
    LlmConfig cfg_;
};

std::unique_ptr<ILlmProvider> make_http_provider(LlmConfig config) {
    return std::make_unique<RealHttpProvider>(std::move(config));
}

LlmConfig llm_config_from_env() {
    LlmConfig cfg;
    if (const char* v = std::getenv("TB_API_BASE")) {
        cfg.api_base = v;
    }
    if (const char* v = std::getenv("TB_API_KEY")) {
        cfg.api_key = v;
    }
    if (const char* v = std::getenv("TB_MODEL")) {
        cfg.model = v;
    }
    return cfg;
}

// ============================================================ prompt 与解析

std::string render_library_prompt(const ComponentLibrary& lib,
                                  const CompositeRegistry& composites) {
    std::ostringstream os;
    os << "你是一个建模指令生成器。只能使用下面列出的指令，不得发明新指令或新参数。\n\n";
    os << "## 可用指令\n";

    for (const auto& [name, cmd] : lib.all()) {
        os << "- " << name;
        if (!cmd.summary.empty()) {
            os << "：" << cmd.summary;
        }
        os << "\n  参数：";
        append_args(os, cmd.args);
        if (!cmd.preconditions.empty()) {
            os << "\n  前置条件：" << cmd.preconditions.front();
        }
        os << "\n";
    }

    if (!composites.names().empty()) {
        os << "\n## 组合件（也是可用的）\n";
        for (const auto& n : composites.names()) {
            const auto* c = composites.find(n);
            os << "- " << n;
            if (c != nullptr && !c->summary.empty()) {
                os << "：" << c->summary;
            }
            if (c != nullptr) {
                os << "\n  参数：";
                append_args(os, c->params);
            }
            os << "\n";
        }
    }

    os << "\n## 输出要求\n";
    os << "1. 只输出一个 JSON 数组，不要任何解释文字，不要 markdown 代码围栏。\n";
    os << "2. 数组元素形如 {\"op\":\"create_circle\",\"args\":{...}}。\n";
    os << "3. 第一条必须是 create_canvas。\n";
    os << "4. 画布默认 400x400，中心在 (200,200)。\n";
    os << "5. 会被其它指令引用的对象要带 id（如 {\"id\":\"c1\",\"op\":\"create_circle\",...}），"
          "create_connect 用 from/to 填这个 id。\n";
    return os.str();
}

std::vector<std::string> extract_instruction_array(const std::string& reply) {
    std::vector<std::string> out;

    // 去掉 ```json ... ``` 围栏
    std::string text = reply;
    const auto fence = text.find("```");
    if (fence != std::string::npos) {
        auto start = text.find('\n', fence);
        if (start != std::string::npos) {
            ++start;
        }
        const auto end = text.find("```", start);
        if (end != std::string::npos) {
            text = text.substr(start, end - start);
        }
    }

    // 找到第一个 [ 和最后一个 ]
    const auto l = text.find('[');
    const auto r = text.rfind(']');
    if (l == std::string::npos || r == std::string::npos || r <= l) {
        return out;
    }
    const std::string arr_text = text.substr(l, r - l + 1);

    boost::json::error_code ec;
    const boost::json::value v = boost::json::parse(arr_text, ec);
    if (ec || !v.is_array()) {
        return out;
    }
    for (const auto& item : v.as_array()) {
        out.push_back(boost::json::serialize(item));
    }
    return out;
}

}  // namespace tb
