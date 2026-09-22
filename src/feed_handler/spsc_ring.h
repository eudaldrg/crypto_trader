// A bounded single-producer single-consumer ring between two threads, a thin
// wrapper over moodycamel::ReaderWriterQueue (decisions/0002).
//
// What the wrapper adds is the policy the queue leaves open: data is bounded
// and lossy, control is unbounded and lossless.
//
//   Push         try_enqueue: never allocates, never blocks. A full ring returns
//                false and counts a drop; the caller decides what a drop means
//                (the book thread marks the book desynced, the journal thread
//                treats it as fatal).
//   PushControl  enqueue: grows the ring if it has to, so it always succeeds.
//                For events whose loss corrupts state (a lost Connect leaves a
//                book built from two different snapshots), which are rare
//                enough that the growth is not a hot-path concern.
//
// Exactly one thread may call Push/PushControl and exactly one Pop; Dropped()
// may be read from any thread.
#pragma once

#include <readerwriterqueue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace feed_handler {

template <typename T>
class SpscRing {
  public:
    /// The ring holds at least `capacity` elements before Push starts failing
    /// (the queue rounds its storage up to a power of two, so it may hold a few
    /// more). PushControl can exceed it.
    explicit SpscRing(std::size_t capacity) : queue_(capacity) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    /// Producer thread only. Returns false and counts a drop when the ring is
    /// full; `value` is left unmoved-from in that case (try_enqueue constructs
    /// the element only on success), so the caller still owns it.
    bool Push(T&& value) {
        if (queue_.try_enqueue(std::move(value))) {
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Producer thread only. Never fails and never counts a drop: allocates
    /// when the ring is full.
    void PushControl(T&& value) {
        queue_.enqueue(std::move(value));
    }

    /// Consumer thread only. Moves the oldest element into `out` and returns
    /// true, or returns false when the ring is empty.
    bool Pop(T& out) {
        return queue_.try_dequeue(out);
    }

    /// How many Push calls returned false so far. Safe from any thread.
    std::uint64_t Dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    moodycamel::ReaderWriterQueue<T> queue_;
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace feed_handler
