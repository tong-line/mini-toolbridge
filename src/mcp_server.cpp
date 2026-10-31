#include "tb/mcp.hpp"

#include <boost/json.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace tb {
namespace {

namespace json = boost::json;

constexpr const char* kServerName = "mini-toolbridge";
constexpr const char* kServerVersion = "0.1.0";
constexpr const char* kDefaultProtocol = "2024-11-05";

std::string serialize(const json::value& v) { return json::serialize(v); }

json::object make_result(const json::value& id, json::value result) {
    json::object out;
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

json::object make_error(const json::value& id, int code, const std::string& message) {
    json::object err;
    err["code"] = code;
    err["message"] = message;
    json::object out;
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = std::move(err);
    return out;
}

// MCP 的 tool 定义：name / description / inputSchema
json::object tool_def(const char* name, const char* desc, json::object schema) {
    json::object t;
    t["name"] = name;
    t["description"] = desc;
    t["inputSchema"] = std::move(schema);
    return t;
}

json::object instructions_array_schema() {
    json::object item;
    item["type"] = "object";
    json::object array;
    array["type"] = "array";
    array["items"] = std::move(item);
    array["description"] = "指令数组，形如 [{\"op\":\"create_canvas\",\"args\":{...}}]";
    return array;
}

// 工具返回：把文本包成 content 数组
json::object text_content(const std::string& text, bool is_error = false) {
    json::object block;
    block["type"] = "text";
    block["text"] = text;
    json::array content;
    content.push_back(std::move(block));

    json::object out;
    out["content"] = std::move(content);
    if (is_error) {
        out["isError"] = true;
    }
    return out;
}

// 把 JSON 值里的数组元素重新序列化成 vector<string>，供 run_instructions 用
bool extract_instructions(const json::value& args, std::string& out_json) {
    if (!args.is_object() || !args.as_object().contains("instructions")) {
        return false;
    }
    const auto& ins = args.as_object().at("instructions");
    if (!ins.is_array()) {
        return false;
    }
    out_json = json::serialize(ins);
    return true;
}

// ---------------------------------------------------------------- 各工具实现

std::string tool_list_components(const ComponentLibrary& lib,
                                 const CompositeRegistry& composites) {
    std::ostringstream os;
    os << "## 基础指令\n";
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
            os << (a.required ? "" : ", 可选") << ")";
        }
        os << "\n";
    }
    if (!composites.names().empty()) {
        os << "\n## 组合件\n";
        for (const auto& n : composites.names()) {
            os << "- " << n << "\n";
        }
    }
    return os.str();
}

json::object call_validate(const json::value& args, const ComponentLibrary& lib,
                           const CompositeRegistry& composites) {
    std::string instructions;
    if (!extract_instructions(args, instructions)) {
        return text_content("arguments 缺少 instructions 数组", true);
    }

    ModelContext ctx;
    SvgExecutor svg;   // 校验通过就会执行，但结果丢弃
    const PipelineStats stats = run_instructions(instructions, lib, composites, ctx, svg);

    std::ostringstream os;
    os << "通过 " << stats.applied << " 条，被拒 " << stats.rejected << " 条\n";
    for (const auto& e : stats.errors) {
        os << "✗ " << e << "\n";
    }
    return text_content(os.str(), stats.rejected != 0);
}

json::object call_render(const json::value& args, const ComponentLibrary& lib,
                         const CompositeRegistry& composites) {
    std::string instructions;
    if (!extract_instructions(args, instructions)) {
        return text_content("arguments 缺少 instructions 数组", true);
    }

    std::string out_path = "toolbridge_mcp.svg";
    if (args.is_object() && args.as_object().contains("output_path") &&
        args.as_object().at("output_path").is_string()) {
        const auto& s = args.as_object().at("output_path").as_string();
        out_path = std::string{s.data(), s.size()};
    }

    ModelContext ctx;
    SvgExecutor svg;
    const PipelineStats stats = run_instructions(instructions, lib, composites, ctx, svg);

    std::ofstream ofs(out_path);
    if (!ofs) {
        return text_content("写文件失败: " + out_path, true);
    }
    ofs << svg.document();
    ofs.close();

    std::ostringstream os;
    os << "通过 " << stats.applied << " 条，被拒 " << stats.rejected << " 条\n";
    for (const auto& e : stats.errors) {
        os << "✗ " << e << "\n";
    }
    os << "产物: " << out_path << "（" << svg.element_count() << " 个元素）\n";
    return text_content(os.str(), stats.rejected != 0);
}

// ---------------------------------------------------------------- 协议分发

json::value handle_tools_call(const json::value& params, const ComponentLibrary& lib,
                              const CompositeRegistry& composites) {
    if (!params.is_object() || !params.as_object().contains("name")) {
        return text_content("tools/call 缺少 name", true);
    }
    const auto& obj = params.as_object();
    const std::string name{obj.at("name").as_string().data(), obj.at("name").as_string().size()};
    const json::value args = obj.contains("arguments") ? obj.at("arguments") : json::value(json::object{});

    if (name == "list_components") {
        return text_content(tool_list_components(lib, composites));
    }
    if (name == "validate_instructions") {
        return call_validate(args, lib, composites);
    }
    if (name == "render_instructions") {
        return call_render(args, lib, composites);
    }
    return text_content("未知工具: " + name, true);
}

}  // namespace

// ============================================================ 对外

std::string mcp_handle_line(const std::string& line, const ComponentLibrary& lib,
                            const CompositeRegistry& composites) {
    if (line.empty()) {
        return {};
    }

    json::error_code ec;
    const json::value msg = json::parse(line, ec);
    if (ec || !msg.is_object()) {
        // 解析失败时没有可用的 id
        return serialize(make_error(json::value(nullptr), -32700, "Parse error"));
    }
    const auto& obj = msg.as_object();

    // 通知（没有 id）不需要回复
    if (!obj.contains("id")) {
        return {};
    }
    const json::value id = obj.at("id");

    if (!obj.contains("method") || !obj.at("method").is_string()) {
        return serialize(make_error(id, -32600, "Invalid Request"));
    }
    const auto& m = obj.at("method").as_string();
    const std::string method{m.data(), m.size()};

    if (method == "initialize") {
        // 回显客户端请求的协议版本，客户端不支持时会自行降级
        std::string version = kDefaultProtocol;
        if (obj.contains("params") && obj.at("params").is_object() &&
            obj.at("params").as_object().contains("protocolVersion")) {
            const auto& v = obj.at("params").as_object().at("protocolVersion");
            if (v.is_string()) {
                version = std::string{v.as_string().data(), v.as_string().size()};
            }
        }
        json::object caps;
        json::object tools_cap;
        caps["tools"] = std::move(tools_cap);

        json::object server_info;
        server_info["name"] = kServerName;
        server_info["version"] = kServerVersion;

        json::object result;
        result["protocolVersion"] = version;
        result["capabilities"] = std::move(caps);
        result["serverInfo"] = std::move(server_info);
        result["instructions"] =
            "把自然语言描述转成受约束的建模指令，校验后渲染为 SVG。"
            "先调 list_components 看可用指令，再调 render_instructions。";
        return serialize(make_result(id, std::move(result)));
    }

    if (method == "ping") {
        return serialize(make_result(id, json::object{}));
    }

    if (method == "tools/list") {
        json::array tools;

        tools.push_back(tool_def(
            "list_components", "列出所有可用的建模指令与参数 schema（不需要参数）",
            [] {
                json::object s;
                s["type"] = "object";
                s["properties"] = json::object{};
                return s;
            }()));

        {
            json::object props;
            props["instructions"] = [] {
                json::object arr;
                arr["type"] = "array";
                json::object item;
                item["type"] = "object";
                arr["items"] = std::move(item);
                arr["description"] = "指令数组";
                return arr;
            }();
            json::object schema;
            schema["type"] = "object";
            schema["properties"] = std::move(props);
            json::array required;
            required.push_back("instructions");
            schema["required"] = std::move(required);
            tools.push_back(tool_def("validate_instructions",
                                     "只校验指令数组，不产生产物", std::move(schema)));
        }

        {
            json::object props;
            props["instructions"] = [] {
                json::object arr;
                arr["type"] = "array";
                json::object item;
                item["type"] = "object";
                arr["items"] = std::move(item);
                arr["description"] = "指令数组";
                return arr;
            }();
            json::object out_prop;
            out_prop["type"] = "string";
            out_prop["description"] = "SVG 输出路径，默认 toolbridge_mcp.svg";
            props["output_path"] = std::move(out_prop);

            json::object schema;
            schema["type"] = "object";
            schema["properties"] = std::move(props);
            json::array required;
            required.push_back("instructions");
            schema["required"] = std::move(required);
            tools.push_back(tool_def("render_instructions",
                                     "校验并执行指令数组，渲染出 SVG 文件", std::move(schema)));
        }

        json::object result;
        result["tools"] = std::move(tools);
        return serialize(make_result(id, std::move(result)));
    }

    if (method == "tools/call") {
        const json::value params =
            obj.contains("params") ? obj.at("params") : json::value(json::object{});
        return serialize(make_result(id, handle_tools_call(params, lib, composites)));
    }

    return serialize(make_error(id, -32601, "Method not found: " + method));
}

void run_mcp_stdio() {
    ComponentLibrary lib = make_display_library();
    CompositeRegistry composites;
    composites.add(make_gear_component());

    std::fprintf(stderr, "[mini-toolbridge] MCP stdio server 已启动\n");
    std::fflush(stderr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string reply = mcp_handle_line(line, lib, composites);
        if (!reply.empty()) {
            std::fwrite(reply.data(), 1, reply.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    }
    std::fprintf(stderr, "[mini-toolbridge] stdin 关闭，退出\n");
}

}  // namespace tb
