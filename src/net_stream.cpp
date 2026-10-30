#include "tb/net.hpp"

#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>

namespace tb {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;
using boost::system::error_code;
using namespace std::chrono_literals;

// ============================================================ URL 解析

struct Url {
    bool tls = false;
    std::string host;
    std::string port;
    std::string target;
};

bool parse_url(const std::string& url, Url& out) {
    std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme == "http") {
        out.tls = false;
        out.port = "80";
    } else if (scheme == "https") {
        out.tls = true;
        out.port = "443";
    } else {
        return false;
    }

    const std::size_t host_begin = scheme_end + 3;
    const std::size_t path_begin = url.find('/', host_begin);
    const std::string authority =
        url.substr(host_begin, path_begin == std::string::npos ? std::string::npos
                                                               : path_begin - host_begin);
    out.target = (path_begin == std::string::npos) ? "/" : url.substr(path_begin);

    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
    }
    return !out.host.empty();
}

// ============================================================ 通用会话

// do_stream 正常退出时取消超时定时器，让超时协程以 operation_aborted 结束、不残留
struct TimerGuard {
    asio::steady_timer* timer = nullptr;
    ~TimerGuard() {
        if (timer) {
            timer->cancel();
        }
    }
};

template <class Stream>
asio::awaitable<HttpStreamResult> do_stream(Stream& stream, const Url& url,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const std::string& body, const ChunkCallback& on_chunk,
                                            std::chrono::steady_clock::duration timeout) {
    HttpStreamResult result;

    // 超时保护：到点取消底层 socket，挂起的读写以 operation_aborted 结束并正常展开协程栈。
    // 正常结束时由 TimerGuard 取消定时器。
    asio::steady_timer timer{stream.get_executor()};
    TimerGuard guard{&timer};
    if (timeout.count() > 0) {
        timer.expires_after(timeout);
        auto& lowest = beast::get_lowest_layer(stream);
        asio::co_spawn(
            stream.get_executor(),
            [&timer, &lowest, &result]() -> asio::awaitable<void> {
                error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    co_return;   // 正常路径取消了定时器
                }
                result.timed_out = true;
                lowest.cancel();   // 无参 cancel：tcp::socket 与 beast::tcp_stream 都可用
            }(),
            asio::detached);
    }

    // 请求：流式响应对应 stream=true 的 LLM 接口
    http::request<http::string_body> req{http::verb::post, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "mini-toolbridge/0.1");
    req.set(http::field::accept, "text/event-stream");
    for (const auto& [k, v] : headers) {
        req.set(k, v);
    }
    req.body() = body;
    req.prepare_payload();

    error_code ec;
    co_await http::async_write(stream, req, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        result.error = "write: " + ec.message();
        co_return result;
    }

    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    co_await http::async_read_header(stream, buffer, parser,
                                     asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        result.error = "read_header: " + ec.message();
        co_return result;
    }
    result.status = static_cast<int>(parser.get().result_int());

    // 逐块读 body，每块立刻交出去 —— 这就是「流式」的全部
    while (!parser.is_done()) {
        char chunk[8192];
        parser.get().body().data = chunk;
        parser.get().body().size = sizeof(chunk);

        co_await http::async_read_some(stream, buffer, parser,
                                       asio::redirect_error(asio::use_awaitable, ec));

        const std::size_t got = sizeof(chunk) - parser.get().body().size;
        if (got > 0) {
            result.bytes += got;
            if (on_chunk) {
                on_chunk(std::string_view(chunk, got));
            }
        }
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            if (ec == asio::error::eof) {
                break;
            }
            result.error = "read_some: " + ec.message();
            co_return result;
        }
    }

    result.ok = (result.status >= 200 && result.status < 300);
    if (!result.ok) {
        result.error = "HTTP " + std::to_string(result.status);
    }
    co_return result;
}

// ============================================================ 明文

asio::awaitable<HttpStreamResult> stream_plain(asio::io_context& ioc, const Url& url,
                                               const std::vector<std::pair<std::string, std::string>>& headers,
                                               const std::string& body, const ChunkCallback& on_chunk,
                                               std::chrono::steady_clock::duration timeout) {
    HttpStreamResult bad;
    error_code ec;
    tcp::resolver resolver{ioc};
    const auto endpoints = co_await resolver.async_resolve(
        url.host, url.port, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        bad.error = "resolve: " + ec.message();
        co_return bad;
    }
    tcp::socket sock{ioc};
    co_await asio::async_connect(sock, endpoints,
                                 asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        bad.error = "connect: " + ec.message();
        co_return bad;
    }
    co_return co_await do_stream(sock, url, headers, body, on_chunk, timeout);
}

// ============================================================ TLS

asio::awaitable<HttpStreamResult> stream_tls(asio::io_context& ioc, const Url& url,
                                             const std::vector<std::pair<std::string, std::string>>& headers,
                                             const std::string& body, const ChunkCallback& on_chunk,
                                             std::chrono::steady_clock::duration timeout) {
    HttpStreamResult bad;
    // 注意：context 必须比 stream 活得久，不能写成临时对象
    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    beast::ssl_stream<beast::tcp_stream> stream{ioc, ssl_ctx};

    error_code ec;
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
        bad.error = "SNI 设置失败";
        co_return bad;
    }
    stream.set_verify_mode(asio::ssl::verify_peer);
    stream.set_verify_callback(asio::ssl::host_name_verification(url.host));

    tcp::resolver resolver{ioc};
    const auto endpoints = co_await resolver.async_resolve(
        url.host, url.port, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        bad.error = "resolve: " + ec.message();
        co_return bad;
    }
    co_await beast::get_lowest_layer(stream).async_connect(
        endpoints, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        bad.error = "connect: " + ec.message();
        co_return bad;
    }
    co_await stream.async_handshake(asio::ssl::stream_base::client,
                                    asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        bad.error = "tls handshake: " + ec.message();
        co_return bad;
    }

    auto r = co_await do_stream(stream, url, headers, body, on_chunk, timeout);

    error_code ignored;
    co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
    co_return r;
}

}  // namespace

// ============================================================ 对外入口

asio::awaitable<HttpStreamResult> http_post_stream(
    asio::io_context& ioc, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
    ChunkCallback on_chunk, std::chrono::steady_clock::duration timeout) {
    Url parsed;
    if (!parse_url(url, parsed)) {
        HttpStreamResult bad;
        bad.error = "URL 非法（需 http:// 或 https://）: " + url;
        co_return bad;
    }
    if (parsed.tls) {
        co_return co_await stream_tls(ioc, parsed, headers, body, on_chunk, timeout);
    }
    co_return co_await stream_plain(ioc, parsed, headers, body, on_chunk, timeout);
}

// ============================================================ mock LLM 服务

namespace {

// 一段预先设计好的"模型输出"，按 token 粒度切开
std::vector<std::string> mock_tokens(bool bad) {
    return {
        "[\n",
        "{\"op\":\"create_canvas\",",
        "\"args\":{\"width\":400,",
        "\"height\":400,",
        "\"bg\":\"#f7f7fa\"}},\n",
        "{\"op\":\"create_circle\",",
        "\"args\":{\"cx\":200,\"cy\":200,",
        "\"r\":170,\"stroke\":\"#1f3b66\",",
        "\"fill\":\"#ffffff\"}},\n",
        "{\"op\":\"create_hand\",",
        "\"args\":{\"angle\":305,",
        "\"length\":95,",
        "\"width\":9,\"stroke\":\"#1f3b66\"}},\n",
        bad ? "{\"op\":\"melt\",\"args\":{}},\n" : "",
        "{\"op\":\"create_hand\",",
        "\"args\":{\"angle\":60,",
        "\"length\":140,",
        "\"width\":6,\"stroke\":\"#c0392b\"}},\n",
        "{\"op\":\"create_circle\",",
        "\"args\":{\"cx\":200,\"cy\":200,\"r\":6,",
        "\"fill\":\"#1f3b66\"}}\n",
        "]\n",
    };
}

std::string chunk_frame(const std::string& payload) {
    std::ostringstream os;
    os << std::hex << payload.size() << "\r\n" << payload << "\r\n";
    return os.str();
}

// 每个连接一个协程
asio::awaitable<void> mock_session(tcp::socket sock, bool bad) {
    error_code ec;
    beast::flat_buffer buffer;

    // 读请求（不关心内容，只为了把请求体读干净）
    http::request_parser<http::string_body> parser;
    parser.body_limit(1024 * 1024);
    co_await http::async_read(sock, buffer, parser,
                              asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        co_return;
    }

    // 手写 SSE 响应头（chunked），随后逐块写
    const std::string head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n";
    co_await asio::async_write(sock, asio::buffer(head),
                               asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        co_return;
    }

    asio::steady_timer timer{sock.get_executor()};
    for (const auto& token : mock_tokens(bad)) {
        if (token.empty()) {
            continue;
        }
        // 造一个 OpenAI 兼容的 SSE 事件：{"choices":[{"delta":{"content":"..."}}]}
        boost::json::object delta;
        delta["content"] = token;
        boost::json::object choice;
        choice["delta"] = std::move(delta);
        boost::json::array choices;
        choices.push_back(std::move(choice));
        boost::json::object event;
        event["choices"] = std::move(choices);

        const std::string payload =
            "data: " + boost::json::serialize(event) + "\n\n";
        const std::string frame = chunk_frame(payload);
        co_await asio::async_write(sock, asio::buffer(frame),
                                   asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }

        timer.expires_after(60ms);
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }

    const std::string done = chunk_frame("data: [DONE]\n\n");
    co_await asio::async_write(sock, asio::buffer(done),
                               asio::redirect_error(asio::use_awaitable, ec));
    const std::string last = "0\r\n\r\n";
    co_await asio::async_write(sock, asio::buffer(last),
                               asio::redirect_error(asio::use_awaitable, ec));
    sock.shutdown(tcp::socket::shutdown_send, ec);
}

asio::awaitable<void> mock_listener(tcp::acceptor& acceptor, bool bad) {
    while (true) {
        error_code ec;
        tcp::socket sock = co_await acceptor.async_accept(
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        // 每个连接独立协程，互不阻塞
        asio::co_spawn(acceptor.get_executor(), mock_session(std::move(sock), bad),
                       asio::detached);
    }
}

}  // namespace

void run_mock_llm(std::uint16_t port, bool bad) {
    asio::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {tcp::v4(), port}};

    std::printf("mock LLM 已启动: http://127.0.0.1:%u/v1/chat/completions\n", port);
    std::printf("（Ctrl-C 退出）%s\n", bad ? "  [bad 模式：第 4 条为非法指令]" : "");

    asio::co_spawn(ioc, mock_listener(acceptor, bad), asio::detached);
    ioc.run();
}

}  // namespace tb
