#pragma once

// 流式指令管道：扫描线程（或 asio 回调）把完整指令 JSON 丢进有界队列，
// worker 线程做校验 + 执行。
//
// 队列满 → 生产者阻塞（反压，不再无界堆积）。
// 校验失败 → worker close 队列 → 生产者 submit 返回 false → 上层断开连接。

#include "tb/queue.hpp"
#include "tb/toolbridge.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace tb {

class InstructionPipeline {
public:
    struct Config {
        std::size_t queue_capacity = 8;
        std::function<void(std::string_view op)> on_applied{};
    };

    InstructionPipeline(const ComponentLibrary& lib, const CompositeRegistry& composites,
                        ModelContext& ctx, SvgExecutor& out, Config cfg);

    ~InstructionPipeline();

    InstructionPipeline(const InstructionPipeline&) = delete;
    InstructionPipeline& operator=(const InstructionPipeline&) = delete;
    InstructionPipeline(InstructionPipeline&&) = delete;
    InstructionPipeline& operator=(InstructionPipeline&&) = delete;

    // 生产者调用。队列满时阻塞。失败或已关闭返回 false。
    bool submit(std::string instruction_json);

    // 关闭生产者端，等待 worker 排空。之后 stats 可安全读取。
    PipelineStats finish();

    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    void worker_loop();

    const ComponentLibrary& lib_;
    const CompositeRegistry& composites_;
    ModelContext& ctx_;
    SvgExecutor& out_;
    Config cfg_;
    BoundedQueue<std::string> queue_;
    std::thread worker_;
    std::atomic<bool> failed_{false};
    PipelineStats stats_;
};

}  // namespace tb
