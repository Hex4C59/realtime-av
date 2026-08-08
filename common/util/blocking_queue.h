#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

// 有界阻塞队列:实时流水线的线程间通道。
// 队列满时 push 丢弃最旧元素——实时通话宁可丢旧帧,也不能让延迟越积越大。
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity) : capacity_(capacity) {}

    // 返回 true 表示本次 push 挤掉了一个旧元素(调用方可据此统计丢帧)
    bool push(T item) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                dropped = true;
            }
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
        return dropped;
    }

    // 阻塞直到有数据;队列关闭且取空后返回 nullopt,消费线程据此退出
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    // 关闭后重新启用(清空残留数据)。调用前必须保证消费线程已退出。
    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        closed_ = false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool closed_ = false;
};
