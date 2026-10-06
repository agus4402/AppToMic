#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

// Lock-free single-producer / single-consumer ring buffer of float samples.
// The capture thread writes, the render thread reads (and may Skip to cap latency).
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : buf_(capacity), cap_(capacity) {}

    size_t Available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    // Producer side.
    size_t Write(const float* src, size_t n) { return Put(src, n); }
    size_t WriteSilence(size_t n) { return Put(nullptr, n); }

    // Consumer side.
    size_t Read(float* dst, size_t n) {
        size_t r = read_.load(std::memory_order_relaxed);
        n = std::min(n, write_.load(std::memory_order_acquire) - r);
        size_t pos = r % cap_;
        size_t first = std::min(n, cap_ - pos);
        memcpy(dst, &buf_[pos], first * sizeof(float));
        memcpy(dst + first, &buf_[0], (n - first) * sizeof(float));
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    void Skip(size_t n) {
        size_t r = read_.load(std::memory_order_relaxed);
        n = std::min(n, write_.load(std::memory_order_acquire) - r);
        read_.store(r + n, std::memory_order_release);
    }

private:
    size_t Put(const float* src, size_t n) {
        size_t w = write_.load(std::memory_order_relaxed);
        n = std::min(n, cap_ - (w - read_.load(std::memory_order_acquire)));
        size_t pos = w % cap_;
        size_t first = std::min(n, cap_ - pos);
        if (src) {
            memcpy(&buf_[pos], src, first * sizeof(float));
            memcpy(&buf_[0], src + first, (n - first) * sizeof(float));
        } else {
            memset(&buf_[pos], 0, first * sizeof(float));
            memset(&buf_[0], 0, (n - first) * sizeof(float));
        }
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    std::vector<float> buf_;
    const size_t cap_;
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};
