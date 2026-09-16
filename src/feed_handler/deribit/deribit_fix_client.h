// Deribit FIX market-data client: the live-socket end of the second exchange
// backend. Drives the pure-logic fix_session (deribit_fix_session.h) over a
// hand-rolled POSIX TCP socket and journals every inbound message through the
// same exchange-agnostic capture_session Kraken uses.
//
// Threading (decisions/0004): one dedicated thread doing a blocking recv()
// loop, which is the same "one thread per connection" scope Kraken's v1 has --
// just hand-rolled instead of library-provided. The epoll-per-thread-group
// model in that ADR is explicitly the end-goal for when there is more than one
// connection to group; building a multiplexer for a single socket would be
// premature. Because this client owns its fd outright (no library in the way,
// unlike IXWebSocket) it can join an epoll group the day one exists.
//
// The recv() has an SO_RCVTIMEO so the one thread can also drive the two timers
// this session needs -- the outbound Heartbeat every HeartBtInt, and the
// application-level staleness watchdog -- without a second thread or a
// readiness API. A timed-out recv is not an event, it is just the loop's tick.
//
// Transport: plain TCP, no TLS. Confirmed against the real testnet by
// experiments/deribit_fix_probe.py (socket.create_connection, no ssl wrap).
//
// Reconnect policy (decisions/0004): a session-level gap, a duplicate, a
// malformed MsgSeqNum, a framing loss or a failed envelope validation all mean
// the same thing -- the session is no longer trustworthy. There is no
// ResendRequest/SequenceReset gap fill; the answer is drop the connection,
// re-logon, take a fresh snapshot, exactly as Kraken reconnects and
// resubscribes. Unlike Kraken there is no library reconnect layer underneath,
// so the backoff here is this client's own and is the only one.
//
// Credentials: the client secret exists only to be hashed into the Logon
// password. Neither it, the password, nor RawData is ever logged, and outbound
// bytes are never journaled -- on_wire_message() is only ever called with bytes
// that came back from recv().
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "feed_handler/capture_session.h"
#include "feed_handler/deribit/deribit_fix_session.h"
#include "feed_handler/fix/fix_message.h"
#include "feed_handler/staleness_watchdog.h"

namespace feed_handler::deribit {

/// What an inbound message is, to the extent this build needs to know. Book
/// content is deliberately not parsed: v1 journals raw bytes, and reading a
/// 35=W/35=X entry list needs the repeating-group parser fix_message.h flags as
/// the corner cut for v1.
enum class inbound_kind : std::uint8_t {
    unknown,
    logon_ack,
    heartbeat,
    test_request,
    logout,
    /// Session-level Reject(35=3): the exchange refused one of our messages.
    session_reject,
    market_data_snapshot,
    market_data_incremental,
    /// MarketDataRequestReject(35=Y): the subscribe failed while the session
    /// stays perfectly healthy and simply never delivers data.
    market_data_request_reject,
};

std::string_view to_string(inbound_kind kind);

/// What the connection loop must do about a message, beyond having already
/// journaled it.
enum class inbound_action : std::uint8_t {
    /// Nothing; the message was journaled and that is the whole job.
    none,
    /// The Logon was accepted -- subscribe.
    send_market_data_request,
    /// Answer a TestRequest with a Heartbeat echoing its TestReqID. Not
    /// optional: an unanswered TestRequest ends the session
    /// (exchanges/deribit.md).
    answer_test_request,
    /// The session can no longer be trusted: drop it and re-logon.
    reconnect,
};

struct inbound_decision {
    inbound_kind kind = inbound_kind::unknown;
    inbound_action action = inbound_action::none;
    /// For `answer_test_request`, the TestReqID(112) to echo. Otherwise a
    /// credential-free reason for the log line (the exchange's own Text(58), or
    /// the sequence check's description). Inbound-derived only, so it can never
    /// carry our secret.
    std::string detail;
};

/// The whole "what happens next" decision for one inbound message, as a pure
/// function of the parsed message and the session's sequence verdict. Split out
/// of the socket loop for the same reason capture_session and
/// staleness_watchdog were: the interesting branches are then testable without
/// a live connection.
inbound_decision classify_inbound(const fix::parsed_message& message, const inbound_check& check);

/// Exponential backoff between connection attempts: 0 before the first one,
/// then `min_ms` doubling up to `max_ms`. There is no library reconnect layer
/// below this client, so this is the only thing standing between a
/// hard-down exchange and a connect() spin loop.
std::uint64_t reconnect_delay_ms(std::uint64_t consecutive_failures, std::uint64_t min_ms,
                                 std::uint64_t max_ms);

struct fix_client_config {
    /// Testnet. Plain TCP, no TLS (experiments/deribit_fix_probe.py).
    std::string host = "fix-test.deribit.com";
    std::uint16_t port = 9881;
    std::string symbol = "BTC-PERPETUAL";
    std::string md_req_id = "ct-md-1";
    /// connect() is done non-blocking + poll() purely so a dead host cannot
    /// hold the thread for the kernel's own multi-minute SYN timeout.
    int connect_timeout_ms = 10'000;
    /// SO_RCVTIMEO. Doubles as the loop's tick for the heartbeat and staleness
    /// timers and as the upper bound on shutdown latency.
    int recv_timeout_ms = 1'000;
    /// No inbound traffic for this long forces a reconnect. Heartbeats every
    /// HeartBtInt(30s) are the expected steady state on an idle book, so this
    /// is deliberately several missed heartbeats rather than a tight bound.
    std::uint64_t staleness_timeout_ns = 90ULL * 1'000'000'000ULL;
    std::uint64_t min_reconnect_wait_ms = 1'000;
    std::uint64_t max_reconnect_wait_ms = 30'000;
};

/// One Deribit FIX connection, owning its own thread and its own fd.
///
/// Not copyable or movable: the thread captures `this`.
class fix_client {
  public:
    fix_client(session_config session_cfg, capture_session& capture, fix_client_config cfg = {});

    fix_client(const fix_client&) = delete;
    fix_client& operator=(const fix_client&) = delete;
    fix_client(fix_client&&) = delete;
    fix_client& operator=(fix_client&&) = delete;
    ~fix_client();

    /// Starts the connection thread and returns immediately.
    void start();

    /// Requests shutdown and joins the thread. The thread sends a Logout on its
    /// way out if it is still logged on -- sending it from the owning thread
    /// rather than from here is what keeps the socket single-threaded.
    /// Idempotent.
    void stop();

    /// True when capture cannot continue (a journal file could not be opened).
    /// The owning process should exit rather than stay connected while
    /// discarding the data it exists to collect.
    bool fatal() const {
        return fatal_.load(std::memory_order_acquire);
    }

    std::uint64_t messages_received() const {
        return messages_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t snapshots_received() const {
        return snapshots_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t incrementals_received() const {
        return incrementals_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t connection_attempts() const {
        return connection_attempts_.load(std::memory_order_relaxed);
    }

    std::uint64_t forced_reconnects() const {
        return forced_reconnects_.load(std::memory_order_relaxed);
    }

  private:
    /// The connection thread: connect, run one session until it ends, back off,
    /// repeat, until stop() or a fatal capture failure.
    void run();
    /// One connection's whole life. Returns when the session has ended for any
    /// reason; the caller decides whether to retry.
    void run_one_connection();
    /// Resolves and connects, non-blocking with a poll() deadline, then puts
    /// the socket back into blocking mode with an SO_RCVTIMEO.
    std::expected<int, std::string> connect_socket();
    /// Drains every whole message the framer can produce, journaling each one
    /// before classifying it. Returns false when the session must be dropped.
    bool drain_framed_messages(int fd);
    /// Journals one framed message verbatim, before any parsing. Returns false
    /// on a journal failure that is fatal to capture.
    bool journal_message(std::string_view raw);
    /// Writes the whole buffer, tolerating short writes and EINTR. Outbound
    /// only -- these bytes never reach the journal.
    bool send_all(int fd, std::string_view bytes);
    /// Returns true if shutdown was requested while waiting.
    bool wait_for_stop(std::uint64_t millis);
    bool stopping() const {
        return stopping_.load(std::memory_order_acquire);
    }

    fix_client_config cfg_;
    capture_session& capture_;
    fix_session session_;
    staleness_watchdog watchdog_;

    /// Everything below is touched only by the connection thread.
    fix::framer framer_;
    bool logged_on_ = false;
    bool subscribed_ = false;
    std::uint64_t last_outbound_ns_ = 0;

    std::thread thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> fatal_{false};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> snapshots_received_{0};
    std::atomic<std::uint64_t> incrementals_received_{0};
    std::atomic<std::uint64_t> connection_attempts_{0};
    std::atomic<std::uint64_t> forced_reconnects_{0};
};

}  // namespace feed_handler::deribit
