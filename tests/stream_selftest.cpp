// 流式链路的自测：SSE 切包 + 增量指令提取。
// 不依赖网络，纯逻辑验证。

#include "tb/stream.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    // 模拟一段 SSE 流，故意让 JSON 对象跨多个 data 事件
    const std::string sse =
        "data: [\n\n"
        "data: {\"op\":\"create_canvas\",\"args\":{\"width\":400,\"height\":400}}\n\n"
        "data: ,{\"op\":\"create_circle\",\"args\":{\"cx\":200,\"cy\":200,\"r\":170}}\n\n"
        "data: ,{\"op\":\"create_hand\",\"args\":{\"angle\":305,\"len\n\n"
        "data: gth\":95}}\n\n"
        "data: ]\n\n"
        "data: [DONE]\n\n";

    tb::SseParser sse_p;
    tb::InstructionScanner scanner;
    int events = 0;
    std::vector<std::string> found;

    auto consume = [&](const std::vector<std::string>& payloads) {
        for (const auto& payload : payloads) {
            ++events;
            if (payload == "[DONE]") {
                continue;
            }
            for (auto& ins : scanner.feed(payload)) {
                found.push_back(ins);
            }
        }
    };

    // 每 7 字节一刀，验证跨包拼接
    for (std::size_t i = 0; i < sse.size(); i += 7) {
        const std::string_view part{sse.data() + i,
                                    std::min<std::size_t>(7, sse.size() - i)};
        consume(sse_p.feed(part));
    }
    consume(sse_p.flush());

    std::printf("SSE 事件数: %d\n", events);
    std::printf("提取到的指令: %zu 条\n", found.size());
    for (const auto& f : found) {
        std::printf("  %s\n", f.c_str());
    }

    const bool ok = found.size() == 3;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
