// The mode-agnostic seam between exchange-facing code (live socket, replay
// journal reader, future simulation engine) and everything downstream of it.
// See decisions/0004-feed-handler-architecture.md, "Mode-agnostic seam".
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace feed_handler {

/// Raw wire bytes exactly as received from an exchange, plus the capture
/// metadata the journal format records.
///
/// Ownership contract (decisions/0004): `payload` is a NON-OWNING view into a
/// buffer that is only guaranteed valid for the duration of the
/// `message_sink::on_frame` call it is passed to. The producer may reuse or
/// free that memory the instant `on_frame` returns, so any sink needing the
/// bytes to outlive the call (e.g. a future SPSC ring feeding another core)
/// must copy them itself before returning.
struct capture_frame {
    std::span<const std::byte> payload;

    /// In-process, per-connection, strictly increasing capture ordinal.
    /// Today this is trivially "write order"; it exists now so Replay-mode
    /// determinism does not need a global ordering scheme retrofitted onto an
    /// already-running journal format later.
    std::uint64_t capture_sequence = 0;

    /// CLOCK_MONOTONIC reading taken when the frame was captured. Correlating
    /// it to wall clock needs the per-file anchor pair in the journal header.
    std::uint64_t monotonic_ns = 0;
};

/// Consumer of capture frames. v1 has exactly one implementation (the journal
/// writer); LiveTrading later adds the order book as a second one.
///
/// Dispatch mechanism (virtual vs. a compile-time policy) is deliberately left
/// open by decisions/0004 until there is a real hot-path sink to measure, so
/// this stays a plain virtual interface for now.
class message_sink {
  public:
    message_sink() = default;
    message_sink(const message_sink&) = default;
    message_sink& operator=(const message_sink&) = default;
    message_sink(message_sink&&) = default;
    message_sink& operator=(message_sink&&) = default;
    virtual ~message_sink() = default;

    /// Called once per inbound wire message, on the connection's own thread.
    virtual void on_frame(const capture_frame& frame) = 0;
};

/// Nanoseconds since an unspecified monotonic epoch (CLOCK_MONOTONIC).
std::uint64_t monotonic_now_ns();

/// Nanoseconds since the Unix epoch (CLOCK_REALTIME).
std::uint64_t realtime_now_ns();

/// Assigns the capture metadata for one connection: the strictly increasing
/// capture sequence number and the CLOCK_MONOTONIC capture timestamp.
///
/// One instance per connection; not thread safe, and does not need to be --
/// each connection stamps its own frames on its own thread
/// (decisions/0004, threading model).
class capture_stamper {
  public:
    /// Returns a frame viewing `payload` (no copy) stamped with the next
    /// sequence number and the current monotonic time. Sequence numbers start
    /// at 1, so 0 is always available as "no frame yet".
    capture_frame stamp(std::span<const std::byte> payload);

    std::uint64_t last_sequence() const {
        return sequence_;
    }

  private:
    std::uint64_t sequence_ = 0;
};

}  // namespace feed_handler
