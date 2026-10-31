#pragma once

// MCP（Model Context Protocol）服务端 —— stdio 传输。
//
// 协议要点：
//   - 每条消息是一行 JSON（换行分隔，消息内不能有裸换行）
//   - stdout 只能输出协议消息；日志一律走 stderr
//   - 生命周期：initialize → notifications/initialized → tools/list / tools/call
//
// 暴露的工具直接映射到组件库：
//   list_components        返回组件库 schema
//   validate_instructions  只校验
//   render_instructions    校验 + 执行 + 写出 SVG

#include "tb/toolbridge.hpp"

#include <string>

namespace tb {

// 处理一行输入，返回需要写回 stdout 的文本（空串表示无需回复，例如通知）
std::string mcp_handle_line(const std::string& line, const ComponentLibrary& lib,
                            const CompositeRegistry& composites);

// 阻塞式 stdio 主循环，直到 stdin EOF
void run_mcp_stdio();

}  // namespace tb
