// The stop-and-fatal signal both capture clients wait on: two latches and the
// wakeup that goes with them.
//
// A client's thread sleeps in timed waits (a watchdog poll, a reconnect
// backoff) and must be woken the moment someone asks it to stop or capture
// fails. Getting that wakeup right is subtle enough that it lives here once,
// with its own test, instead of being re-derived in every client.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace feed_handler {

class StopSignal {
  public:
    /// Asks to stop and wakes every waiter. Returns true if this call made the
    /// request, false if it had already been made. Idempotent.
    bool RequestStop() {
        {
            // The flag is flipped under the mutex a waiter evaluates its
            // predicate under. Flipped outside it, the store can land between a
            // waiter's evaluation and its wait registering, the notification
            // then reaches nobody, and the waiter sleeps out its whole timeout
            // (up to a reconnect backoff).
            const std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
                return false;
            }
        }
        // Notified after unlocking: the new state is already published, so this
        // only saves the woken thread from waking onto a mutex still held here.
        cv_.notify_all();
        return true;
    }

    bool StopRequested() const {
        return stop_requested_.load(std::memory_order_acquire);
    }

    /// Latches a capture failure (a journal that cannot be opened or written)
    /// and wakes every waiter, for the same reasons and in the same way as
    /// RequestStop(). Callers must not hold the mutex, which they cannot: it is
    /// private and only ever held inside this class.
    void LatchFatal() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            fatal_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    bool Fatal() const {
        return fatal_.load(std::memory_order_acquire);
    }

    /// Sleeps up to `timeout`, ending early when a stop is requested or a fatal
    /// error is latched. Returns true in either case: both mean "stop what you
    /// were about to do". Every timed wait in a client goes through here, which
    /// is what makes the notify on the fatal path actually end them.
    bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return StopRequested() || Fatal(); });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> fatal_{false};
};

}  // namespace feed_handler
