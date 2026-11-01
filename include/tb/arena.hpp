#pragma once

// 块式 bump arena：热路径上 intern 字符串、分配短生命周期对象。
// reset() 只回绕指针，块保留，避免反复向系统要内存。

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace tb {

// 透明哈希：unordered_set<string_view> 可以用 string / string_view 直接查找，不造临时 key。
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

class Arena {
public:
    explicit Arena(std::size_t block_size = 4096)
        : block_size_(block_size < 64 ? 64 : block_size) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept { move_from(other); }
    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            blocks_.clear();
            block_caps_.clear();
            move_from(other);
        }
        return *this;
    }

    ~Arena() = default;

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) {
            bytes = 1;
        }
        if (alignment < 1) {
            alignment = 1;
        }

        void* p = current_;
        std::size_t space = static_cast<std::size_t>(current_end_ - current_);
        if (current_ == nullptr || std::align(alignment, bytes, p, space) == nullptr) {
            new_block(bytes + alignment);
            p = current_;
            space = static_cast<std::size_t>(current_end_ - current_);
            if (std::align(alignment, bytes, p, space) == nullptr) {
                return nullptr;
            }
        }
        current_ = static_cast<char*>(p) + bytes;
        used_ += bytes;
        return p;
    }

    // 拷进 arena，返回指向块内的 view。空串不分配。
    std::string_view intern(std::string_view s) {
        if (s.empty()) {
            return {};
        }
        char* p = static_cast<char*>(allocate(s.size() + 1, 1));
        if (p == nullptr) {
            return {};
        }
        std::memcpy(p, s.data(), s.size());
        p[s.size()] = '\0';
        return {p, s.size()};
    }

    void reset() {
        used_ = 0;
        if (blocks_.empty()) {
            current_ = nullptr;
            current_end_ = nullptr;
            return;
        }
        current_ = blocks_.front().get();
        current_end_ = current_ + block_caps_.front();
        rewind_index_ = 0;
    }

    std::size_t used() const { return used_; }
    std::size_t block_count() const { return blocks_.size(); }

private:
    void new_block(std::size_t min_size) {
        // reset 之后再次分配：先复用已有块，用完再向系统要。
        if (rewind_index_ + 1 < blocks_.size()) {
            ++rewind_index_;
            current_ = blocks_[rewind_index_].get();
            current_end_ = current_ + block_caps_[rewind_index_];
            if (static_cast<std::size_t>(current_end_ - current_) >= min_size) {
                return;
            }
        }
        const std::size_t cap = std::max(block_size_, min_size);
        auto block = std::make_unique<char[]>(cap);
        current_ = block.get();
        current_end_ = current_ + cap;
        blocks_.push_back(std::move(block));
        block_caps_.push_back(cap);
        rewind_index_ = blocks_.size() - 1;
    }

    void move_from(Arena& other) noexcept {
        blocks_ = std::move(other.blocks_);
        block_caps_ = std::move(other.block_caps_);
        block_size_ = other.block_size_;
        current_ = other.current_;
        current_end_ = other.current_end_;
        used_ = other.used_;
        rewind_index_ = other.rewind_index_;
        other.current_ = nullptr;
        other.current_end_ = nullptr;
        other.used_ = 0;
        other.rewind_index_ = 0;
    }

    std::vector<std::unique_ptr<char[]>> blocks_;
    std::vector<std::size_t> block_caps_;
    std::size_t block_size_ = 4096;
    char* current_ = nullptr;
    char* current_end_ = nullptr;
    std::size_t used_ = 0;
    std::size_t rewind_index_ = 0;
};

}  // namespace tb
