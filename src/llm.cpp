#include "tb/toolbridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <unistd.h>

namespace tb {
namespace {

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

// ============================================================ curl provider
//
// 用 curl 子进程调任意 OpenAI 兼容端点，避免引入 TLS 依赖。
// 环境变量：
//   TB_API_BASE  例 https://api.deepseek.com/v1
//   TB_API_KEY   密钥
//   TB_MODEL     例 deepseek-chat
//
// ⚠️ 密钥通过环境变量传给 curl，不出现在命令行参数里（ps 看不到）。

// 正确的做法：把请求体写进临时文件，用 popen 只读响应
LlmReply curl_complete(LlmConfig& cfg, const std::string& system_prompt,
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

    // 请求体写到临时文件
    const char* tmp_template = "/tmp/tb_req_XXXXXX";
    char tmp_path[64];
    std::snprintf(tmp_path, sizeof tmp_path, "%s", tmp_template);
    const int fd = ::mkstemp(tmp_path);
    if (fd < 0) {
        return LlmReply{false, "", "无法创建临时文件"};
    }
    std::FILE* f = ::fdopen(fd, "w");
    if (f == nullptr) {
        ::close(fd);
        return LlmReply{false, "", "fdopen 失败"};
    }
    std::fwrite(payload.data(), 1, payload.size(), f);
    std::fclose(f);

    std::ostringstream cmd;
    cmd << "curl -sS --max-time " << cfg.timeout_sec
        << " -X POST \"${TB_API_BASE}/chat/completions\""
        << " -H \"Content-Type: application/json\""
        << " -H \"Authorization: Bearer ${TB_API_KEY}\""
        << " --data-binary @" << tmp_path;

    std::string response;
    if (std::FILE* pipe = ::popen(cmd.str().c_str(), "r")) {
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0) {
            response.append(buf, n);
        }
        ::pclose(pipe);
    }
    std::remove(tmp_path);

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

class RealCurlProvider : public ILlmProvider {
public:
    explicit RealCurlProvider(LlmConfig cfg) : cfg_(std::move(cfg)) {}
    LlmReply complete(const std::string& system_prompt, const std::string& user_prompt) override {
        return curl_complete(cfg_, system_prompt, user_prompt);
    }

private:
    LlmConfig cfg_;
};

std::unique_ptr<ILlmProvider> make_curl_provider(LlmConfig config) {
    return std::make_unique<RealCurlProvider>(std::move(config));
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
        bool first = true;
        for (const auto& a : cmd.args) {
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
            os << "\n";
        }
    }

    os << "\n## 输出要求\n";
    os << "1. 只输出一个 JSON 数组，不要任何解释文字，不要 markdown 代码围栏。\n";
    os << "2. 数组元素形如 {\"op\":\"create_circle\",\"args\":{...}}。\n";
    os << "3. 第一条必须是 create_canvas。\n";
    os << "4. 画布默认 400x400，中心在 (200,200)。\n";
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
