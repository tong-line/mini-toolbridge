#pragma once

// 有界阻塞队列：生产者在满时等待（反压），close 后双方都被唤醒。
// 所有权：push 用移动语义把元素交给队列，pop 再移出。

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace tb {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : cap_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 队列已 close 时返回 false，item 被丢弃（已移入的参数在失败路径析构）。
    bool push(T item) {
        std::unique_lock lock(mu_);
        cv_not_full_.wait(lock, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) {
            return false;
        }
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    // close 且排空后返回 nullopt。
    std::optional<T> pop() {
        std::unique_lock lock(mu_);
        cv_not_empty_.wait(lock, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) {
            return std::nullopt;
        }
        T item = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return item;
    }

    void close() {
        std::lock_guard lock(mu_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    bool closed() const {
        std::lock_guard lock(mu_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard lock(mu_);
        return q_.size();
    }

    std::size_t capacity() const { return cap_; }

private:
    const std::size_t cap_;
    mutable std::mutex mu_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::queue<T> q_;
    bool closed_ = false;
};

}  // namespace tb
