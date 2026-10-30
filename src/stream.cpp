#include "tb/stream.hpp"

namespace tb {

// ============================================================ SSE

std::vector<std::string> SseParser::feed(std::string_view chunk) {
    std::vector<std::string> out;
    line_buf_.append(chunk);

    std::size_t start = 0;
    while (true) {
        const std::size_t nl = line_buf_.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = line_buf_.substr(start, nl - start);
        start = nl + 1;

        // 去掉可能的 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            // 事件结束
            if (!event_data_.empty()) {
                out.push_back(event_data_);
                event_data_.clear();
            }
            continue;
        }

        // 只关心 data: 行；注释行（: 开头）与其它字段忽略
        if (line.rfind("data:", 0) == 0) {
            std::string_view payload{line};
            payload.remove_prefix(5);
            if (!payload.empty() && payload.front() == ' ') {
                payload.remove_prefix(1);
            }
            event_data_.append(payload);
        }
        // 其它字段（event:/id:/retry:）本 demo 不处理
    }

    line_buf_.erase(0, start);
    return out;
}

std::vector<std::string> SseParser::flush() {
    std::vector<std::string> out;
    if (!line_buf_.empty()) {
        // 最后一行没有换行符，也当成一行处理
        const std::string line = line_buf_;
        line_buf_.clear();
        if (line.rfind("data:", 0) == 0) {
            std::string_view payload{line};
            payload.remove_prefix(5);
            if (!payload.empty() && payload.front() == ' ') {
                payload.remove_prefix(1);
            }
            event_data_.append(payload);
        }
    }
    if (!event_data_.empty()) {
        out.push_back(event_data_);
        event_data_.clear();
    }
    return out;
}

// ============================================================ 增量指令提取

std::vector<std::string> InstructionScanner::feed(std::string_view chunk) {
    std::vector<std::string> out;
    buf_.append(chunk);

    for (; pos_ < buf_.size(); ++pos_) {
        const char c = buf_[pos_];

        if (in_string_) {
            if (escaped_) {
                escaped_ = false;
            } else if (c == '\\') {
                escaped_ = true;
            } else if (c == '"') {
                in_string_ = false;
            }
            continue;
        }

        if (c == '"') {
            in_string_ = true;
            continue;
        }

        if (c == '{') {
            if (depth_ == 0) {
                obj_start_ = pos_;
            }
            ++depth_;
            continue;
        }

        if (c == '}') {
            if (depth_ == 0) {
                continue;   // 不配对，忽略
            }
            --depth_;
            if (depth_ == 0) {
                out.emplace_back(buf_.substr(obj_start_, pos_ - obj_start_ + 1));
            }
            continue;
        }
        // 其它字符（含 [ ] , 空白）都是分隔符，不影响配对
    }

    return out;
}

}  // namespace tb
