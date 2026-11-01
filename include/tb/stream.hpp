#pragma once

// 流式链路的两块纯逻辑（不涉及 I/O，可以单独测）：
//   SseParser           —— 把字节流切成 SSE 的 data 负载
//   InstructionScanner  —— 边累积文本边抠出「已完整的」指令对象
//
// 为什么需要后者：大模型返回的是一个 JSON 数组，但流式到达时
// 数组可能只到一半。等到整个数组收完再解析，等于放弃流式的全部好处。
// 正确做法是：每凑齐一个完整的 {...} 就立刻送去校验和执行。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tb {

// SSE 解析：按行累积，空行触发一次事件。容忍任意位置切分。
class SseParser {
public:
    // 喂入一段字节，返回本次新产生的 data 负载（可能为空）
    std::vector<std::string> feed(std::string_view chunk);

    // 流结束时调用，吐出尚未触发事件的残留
    std::vector<std::string> flush();

private:
    std::string line_buf_;      // 未成行的残留
    std::string event_data_;    // 当前事件的 data 累积
};

// 增量指令提取：从字节流里抠出完整的顶层 JSON 对象。
//
// 只关心 {} 的配对与字符串转义，忽略 [] —— 因为我们找的是数组里的每个对象。
// 这样即使一个对象被切成三段到达，也能正确拼回。
class InstructionScanner {
public:
    // 喂入一段文本，返回本次新抠出的完整指令 JSON（每个元素是一段独立文本）
    std::vector<std::string> feed(std::string_view chunk);

    // 累计喂入的字符数（compact 不影响）
    std::size_t total_chars() const { return total_fed_; }
    // 当前缓冲区占用（已吐出的对象会被 compact 丢掉）
    std::size_t buffered() const { return buf_.size(); }

private:
    void compact();

    std::string buf_;
    std::size_t pos_ = 0;        // 扫描游标（已处理到的位置）
    std::size_t obj_start_ = 0;  // 当前对象的起始偏移
    std::size_t total_fed_ = 0;
    int depth_ = 0;              // {} 嵌套深度
    bool in_string_ = false;
    bool escaped_ = false;
};

}  // namespace tb
