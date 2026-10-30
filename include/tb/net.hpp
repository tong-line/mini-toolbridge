#pragma once

// 异步网络层：流式 POST + 本地 mock SSE 服务。
//
// 关键点：
//   - 用 C++20 协程（asio::awaitable），读流不占线程
//   - 响应体边收边回调，不等完整 body
//   - 支持 http:// 与 https://（TLS 走 OpenSSL）

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tb {

struct HttpStreamResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::size_t bytes = 0;
    bool timed_out = false;   // 超时中断
};

using ChunkCallback = std::function<void(std::string_view)>;

// 发起 POST，响应体每到达一段就调用 on_chunk。
// url 形如 http://127.0.0.1:8081/v1/chat/completions 或 https://api.deepseek.com/v1/...
// timeout 为 0 时不设超时；否则到点取消底层 socket，挂起的操作以 operation_aborted
// 结束、协程栈正常展开，并置 timed_out。
boost::asio::awaitable<HttpStreamResult> http_post_stream(
    boost::asio::io_context& ioc,
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    ChunkCallback on_chunk,
    std::chrono::steady_clock::duration timeout = std::chrono::steady_clock::duration::zero());

// 本地 mock LLM：监听 port，对任意 POST 返回一段分块的 SSE 流。
// 用于在没有 API key 的情况下验证整条流式链路。
// bad=true 时会在第 3 条给出非法指令，用于演示「流式校验提前中止」
void run_mock_llm(std::uint16_t port, bool bad = false);

}  // namespace tb
