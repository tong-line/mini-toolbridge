// MCP 协议层自测：不启动子进程，直接喂 JSON-RPC 消息。
// 覆盖：initialize / tools/list / tools/call（三个工具）/ 未知方法 / 通知不回复。

#include "tb/mcp.hpp"
#include "tb/toolbridge.hpp"

#include <boost/json.hpp>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "  ✗ %s\n", what);
        ++g_failures;
    }
}

boost::json::object parse_obj(const std::string& text) {
    boost::json::error_code ec;
    const auto v = boost::json::parse(text, ec);
    if (ec || !v.is_object()) {
        return {};
    }
    return v.as_object();
}

}  // namespace

int main() {
    tb::ComponentLibrary lib = tb::make_display_library();
    tb::CompositeRegistry composites;
    composites.add(tb::make_gear_component());

    // 1) initialize
    {
        const std::string reply = tb::mcp_handle_line(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
            lib, composites);
        const auto obj = parse_obj(reply);
        check(!obj.empty(), "initialize 应有回复");
        check(obj.contains("result"), "initialize 应含 result");
        if (obj.contains("result")) {
            const auto& r = obj.at("result").as_object();
            check(r.contains("protocolVersion"), "应回显 protocolVersion");
            check(r.at("protocolVersion").as_string() == "2025-06-18", "协议版本应回显");
            check(r.contains("serverInfo"), "应含 serverInfo");
            check(r.at("serverInfo").as_object().at("name").as_string() == "mini-toolbridge",
                  "serverInfo.name 正确");
        }
    }

    // 2) 通知不应回复
    {
        const std::string reply = tb::mcp_handle_line(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", lib, composites);
        check(reply.empty(), "通知不应有回复");
    }

    // 3) tools/list：应有三个工具
    {
        const std::string reply =
            tb::mcp_handle_line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("result"), "tools/list 应有 result");
        if (obj.contains("result")) {
            const auto& tools = obj.at("result").as_object().at("tools").as_array();
            check(tools.size() == 3, "应有 3 个工具");
            bool has_render = false;
            for (const auto& t : tools) {
                if (t.as_object().at("name").as_string() == "render_instructions") {
                    has_render = true;
                }
                check(t.as_object().contains("inputSchema"), "每个工具都要有 inputSchema");
            }
            check(has_render, "应包含 render_instructions");
        }
    }

    // 4) tools/call: list_components
    {
        const std::string reply = tb::mcp_handle_line(
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_components","arguments":{}}})",
            lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("result"), "list_components 应有 result");
        if (obj.contains("result")) {
            const auto& r = obj.at("result").as_object();
            check(r.contains("content"), "应含 content");
            const auto& text = r.at("content").as_array().front().as_object().at("text").as_string();
            check(text.find("create_canvas") != boost::json::string_view::npos,
                  "组件清单应包含 create_canvas");
        }
    }

    // 5) tools/call: render_instructions 正常
    {
        const std::string reply = tb::mcp_handle_line(
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"render_instructions","arguments":{"instructions":[{"op":"create_canvas","args":{"width":200,"height":200}},{"op":"create_circle","args":{"cx":100,"cy":100,"r":50}}],"output_path":"/tmp/mcp_test.svg"}}})",
            lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("result"), "render 应有 result");
        if (obj.contains("result")) {
            const auto& r = obj.at("result").as_object();
            check(!r.contains("isError"), "正常渲染不应是 error");
            const auto& text = r.at("content").as_array().front().as_object().at("text").as_string();
            check(text.find("通过 2 条") != boost::json::string_view::npos, "应报告通过 2 条");
            check(text.find("/tmp/mcp_test.svg") != boost::json::string_view::npos, "应报告产物路径");
        }
    }

    // 6) tools/call: 非法指令 → isError
    {
        const std::string reply = tb::mcp_handle_line(
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"render_instructions","arguments":{"instructions":[{"op":"melt","args":{}}]}}})",
            lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("result"), "非法指令仍有 result（工具层错误）");
        if (obj.contains("result")) {
            const auto& r = obj.at("result").as_object();
            check(r.contains("isError") && r.at("isError").as_bool(), "非法指令应标记 isError");
        }
    }

    // 7) 未知方法 → -32601
    {
        const std::string reply =
            tb::mcp_handle_line(R"({"jsonrpc":"2.0","id":6,"method":"nope"})", lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("error"), "未知方法应返回 error");
        if (obj.contains("error")) {
            check(obj.at("error").as_object().at("code").as_int64() == -32601, "错误码应为 -32601");
        }
    }

    // 8) 坏 JSON → -32700
    {
        const std::string reply = tb::mcp_handle_line("{not json", lib, composites);
        const auto obj = parse_obj(reply);
        check(obj.contains("error"), "坏 JSON 应返回 error");
        if (obj.contains("error")) {
            check(obj.at("error").as_object().at("code").as_int64() == -32700, "错误码应为 -32700");
        }
    }

    if (g_failures == 0) {
        std::printf("PASS（8 组断言）\n");
        return 0;
    }
    std::printf("FAIL: %d 项\n", g_failures);
    return 1;
}
