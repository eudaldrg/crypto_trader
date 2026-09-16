// The mode-agnostic seam between exchange-facing code (live socket, replay
// journal reader, future simulation engine) and everything downstream of it.
// See decisions/0004-feed-handler-architecture.md, "Mode-agnostic seam".
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace feed_handler {

/// Which connection produced a frame, and therefore what shape its bytes are
/// in. Named after the wire encoding rather than just the exchange, because
/// what a sink actually has to decide is which parser the payload goes to.
///
/// This exists so a sink fed by more than one connection -- the order book,
/// once it exists -- can branch without re-deriving the answer from the raw
/// bytes it was handed.
///
/// Deliberately NOT part of the on-disk journal format: a journal file is one
/// per (exchange, connection-incarnation) and its header already carries the
/// exchange tag, so a replay source recovers this once per file rather than
/// once per record, and the format needs no version bump for it
/// (journal_format.h).
enum class frame_source : std::uint8_t {
    /// Not stated. Nothing on a live path produces this; it is what a
    /// default-constructed frame carries.
    unknown = 0,
    /// Kraken WebSocket v2 JSON text (kraken/kraken_ws_client.h).
    kraken_json = 1,
    /// Deribit FIX.4.4 tag=value bytes (deribit/deribit_fix_client.h).
    deribit_fix = 2,
};

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

    /// Which connection/wire encoding this payload came off. Set by the
    /// exchange client at the capture call site, since that is the only place
    /// that knows first-hand what it just received.
    frame_source source = frame_source::unknown;
};

/// Consumer of capture frames: the journal writer, and -- once it exists --
/// the order book. capture_session fans one frame out to the journal writer
/// plus any number of registered sinks.
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

    /// Called when a new connection incarnation begins, before any of its
    /// frames: "the connection was re-established, a fresh snapshot follows,
    /// throw away whatever you built from the previous one". `reason` is the
    /// same free-form text journaled in the incarnation marker record.
    ///
    /// This is the one event a stateful sink cannot be correct without -- a
    /// reconnect is exactly when an order book has to reset -- but most sinks
    /// have no state to reset, hence a no-op default rather than a second pure
    /// virtual every implementation would have to write out.
    virtual void on_incarnation(std::uint64_t /*incarnation*/, std::string_view /*reason*/) {}
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
    /// sequence number, the current monotonic time and `source`. Sequence
    /// numbers start at 1, so 0 is always available as "no frame yet".
    ///
    /// `source` defaults to unknown for the benefit of writer-level tests: the
    /// journal format does not record it, so a test exercising the on-disk
    /// bytes has nothing to say about it. Every live capture path states it.
    capture_frame stamp(std::span<const std::byte> payload,
                        frame_source source = frame_source::unknown);

    std::uint64_t last_sequence() const {
        return sequence_;
    }

  private:
    std::uint64_t sequence_ = 0;
};

}  // namespace feed_handler
