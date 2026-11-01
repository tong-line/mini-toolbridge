#include "tb/pipeline.hpp"

#include "tb/toolbridge.hpp"

namespace tb {

InstructionPipeline::InstructionPipeline(const ComponentLibrary& lib,
                                         const CompositeRegistry& composites, ModelContext& ctx,
                                         SvgExecutor& out, Config cfg)
    : lib_(lib),
      composites_(composites),
      ctx_(ctx),
      out_(out),
      cfg_(std::move(cfg)),
      queue_(cfg_.queue_capacity) {
    worker_ = std::thread([this] { worker_loop(); });
}

InstructionPipeline::~InstructionPipeline() {
    queue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool InstructionPipeline::submit(std::string instruction_json) {
    if (failed_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!queue_.push(std::move(instruction_json))) {
        return false;
    }
    return !failed_.load(std::memory_order_acquire);
}

PipelineStats InstructionPipeline::finish() {
    queue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    return stats_;
}

void InstructionPipeline::worker_loop() {
    while (auto item = queue_.pop()) {
        ApplyOutcome outcome;
        try {
            outcome = apply_one_text(*item, lib_, composites_, ctx_, out_);
        } catch (...) {
            failed_.store(true, std::memory_order_release);
            stats_.errors.emplace_back("[内部] worker 异常");
            ++stats_.rejected;
            queue_.close();
            return;
        }
        if (outcome.ok) {
            ++stats_.applied;
            if (cfg_.on_applied) {
                cfg_.on_applied(outcome.op);
            }
            continue;
        }
        ++stats_.rejected;
        stats_.errors.push_back(std::move(outcome.error));
        failed_.store(true, std::memory_order_release);
        queue_.close();
        return;
    }
}

}  // namespace tb
