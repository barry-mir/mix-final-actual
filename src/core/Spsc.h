#pragma once
// Lock-free single-producer/single-consumer ring buffer. Storage is allocated
// in resize() (call off the audio thread); push/pop never allocate or lock.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace im
{
template <typename T>
class SpscRing
{
public:
    void resize (size_t capacityPow2Min)
    {
        size_t cap = 1;
        while (cap < capacityPow2Min + 1) cap <<= 1;
        buf.assign (cap, T {});
        mask = cap - 1;
        head.store (0);
        tail.store (0);
    }
    size_t capacity() const { return buf.empty() ? 0 : buf.size() - 1; }

    size_t availableToRead() const noexcept
    {
        return (head.load (std::memory_order_acquire) - tail.load (std::memory_order_relaxed)) & mask;
    }
    size_t availableToWrite() const noexcept
    {
        if (buf.empty()) return 0;
        return capacity() - ((head.load (std::memory_order_relaxed) - tail.load (std::memory_order_acquire)) & mask);
    }

    // Writes all n items or none (so a batch becomes visible atomically). Returns false if full.
    bool pushAll (const T* items, size_t n) noexcept
    {
        if (n > availableToWrite()) return false;
        size_t h = head.load (std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf[(h + i) & mask] = items[i];
        head.store ((h + n) & mask, std::memory_order_release);
        return true;
    }
    bool push (const T& item) noexcept { return pushAll (&item, 1); }

    // Writes as many as fit; returns count written.
    size_t pushSome (const T* items, size_t n) noexcept
    {
        const size_t w = std::min (n, availableToWrite());
        size_t h = head.load (std::memory_order_relaxed);
        for (size_t i = 0; i < w; ++i) buf[(h + i) & mask] = items[i];
        head.store ((h + w) & mask, std::memory_order_release);
        return w;
    }

    size_t pop (T* out, size_t maxN) noexcept
    {
        const size_t n = std::min (maxN, availableToRead());
        size_t t = tail.load (std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) out[i] = buf[(t + i) & mask];
        tail.store ((t + n) & mask, std::memory_order_release);
        return n;
    }
    bool pop (T& out) noexcept { return pop (&out, 1) == 1; }

private:
    std::vector<T> buf;
    size_t mask = 0;
    alignas (64) std::atomic<size_t> head { 0 };
    alignas (64) std::atomic<size_t> tail { 0 };
};
} // namespace im
