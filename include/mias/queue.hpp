#pragma once
#include "mias/camera.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace mias {
struct QueuedFrame { Frame frame; std::uint64_t capture_index = 0; };
class FrameQueue {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedFrame> queue_;
    std::size_t max_frames_, max_bytes_, bytes_ = 0, high_water_ = 0;
    bool closed_ = false;
public:
    FrameQueue(std::size_t frames, std::size_t bytes) : max_frames_(frames), max_bytes_(bytes) {}
    bool push(QueuedFrame item) {
        const auto bytes = item.frame.image.total() * item.frame.image.elemSize();
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= max_frames_ || bytes > max_bytes_ - bytes_) return false;
        queue_.push_back(std::move(item)); bytes_ += bytes;
        high_water_ = std::max(high_water_, queue_.size()); ready_.notify_one(); return true;
    }
    bool pop(QueuedFrame& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        item = std::move(queue_.front()); queue_.pop_front();
        bytes_ -= item.frame.image.total() * item.frame.image.elemSize(); return true;
    }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; ready_.notify_all(); }
    std::size_t high_water() { std::lock_guard<std::mutex> lock(mutex_); return high_water_; }
};
} // namespace mias
