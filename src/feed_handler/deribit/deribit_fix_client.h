// Deribit FIX market-data client: the live-socket end of the second exchange
// backend. Drives the pure-logic FixSession (deribit_fix_session.h) over a
// hand-rolled POSIX TCP socket and journals every inbound message through the
// same exchange-agnostic CaptureSession Kraken uses.
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
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/deribit/deribit_fix_session.h"
#include "feed_handler/fix/fix_message.h"
#include "feed_handler/staleness_watchdog.h"

namespace feed_handler::deribit {

/// What every frame this client captures is: Deribit FIX.4.4 tag=value bytes.
///
/// Stamped onto each frame by this client rather than taken from the
/// CaptureSession's configuration, for the same reason Kraken's equivalent is
/// (kraken_ws_client.h): the client is the only thing that knows first-hand
/// what shape the bytes it just received are in.
inline constexpr FrameSource kWireSource = FrameSource::kDeribitFix;

/// What an inbound message is, to the extent this build needs to know. Book
/// content is deliberately not parsed here even though `fix::ReadGroup()` can
/// now read a 35=W/35=X entry list: journaling raw bytes is v1's whole job,
/// and turning entries into order-book state is future work, not a parser
/// limitation anymore (fix_message.h).
enum class InboundKind : std::uint8_t {
    kUnknown,
    kLogonAck,
    kHeartbeat,
    kTestRequest,
    kLogout,
    /// Session-level Reject(35=3): the exchange refused one of our messages.
    kSessionReject,
    kMarketDataSnapshot,
    kMarketDataIncremental,
    /// MarketDataRequestReject(35=Y): the subscribe failed while the session
    /// stays perfectly healthy and simply never delivers data.
    kMarketDataRequestReject,
};

std::string_view ToString(InboundKind kind);

/// What the connection loop must do about a message, beyond having already
/// journaled it.
enum class InboundAction : std::uint8_t {
    /// Nothing; the message was journaled and that is the whole job.
    kNone,
    /// The Logon was accepted -- subscribe.
    kSendMarketDataRequest,
    /// Answer a TestRequest with a Heartbeat echoing its TestReqID. Not
    /// optional: an unanswered TestRequest ends the session
    /// (exchanges/deribit.md).
    kAnswerTestRequest,
    /// The session can no longer be trusted: drop it and re-logon.
    kReconnect,
};

struct InboundDecision {
    InboundKind kind = InboundKind::kUnknown;
    InboundAction action = InboundAction::kNone;
    /// For `answer_test_request`, the TestReqID(112) to echo. Otherwise a
    /// credential-free reason for the log line (the exchange's own Text(58), or
    /// the sequence check's description). Inbound-derived only, so it can never
    /// carry our secret.
    std::string detail;
};

/// The whole "what happens next" decision for one inbound message, as a pure
/// function of the parsed message and the session's sequence verdict. Split out
/// of the socket loop for the same reason CaptureSession and
/// StalenessWatchdog were: the interesting branches are then testable without
/// a live connection.
InboundDecision ClassifyInbound(const fix::ParsedMessage& message, const InboundCheck& check);

/// Exponential backoff between connection attempts: 0 before the first one,
/// then `min_ms` doubling up to `max_ms`. There is no library reconnect layer
/// below this client, so this is the only thing standing between a
/// hard-down exchange and a connect() spin loop.
std::uint64_t ReconnectDelayMs(std::uint64_t consecutive_failures, std::uint64_t min_ms,
                               std::uint64_t max_ms);

struct FixClientConfig {
    /// The connection's identity in log lines, so several connections in one
    /// process can be told apart. The config's `id`.
    std::string id = "deribit";
    /// Testnet. Plain TCP, no TLS (experiments/deribit_fix_probe.py).
    std::string host = "fix-test.deribit.com";
    std::uint16_t port = 9881;
    /// All requested in one MarketDataRequest on this one session.
    std::vector<std::string> symbols = {"BTC-PERPETUAL"};
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
class FixClient {
  public:
    FixClient(SessionConfig session_cfg, CaptureSession& capture, FixClientConfig cfg = {});

    FixClient(const FixClient&) = delete;
    FixClient& operator=(const FixClient&) = delete;
    FixClient(FixClient&&) = delete;
    FixClient& operator=(FixClient&&) = delete;
    ~FixClient();

    /// Starts the connection thread and returns immediately.
    void Start();

    /// Requests shutdown and joins the thread. The thread sends a Logout on its
    /// way out if it is still logged on -- sending it from the owning thread
    /// rather than from here is what keeps the socket single-threaded.
    /// Idempotent.
    void Stop();

    /// True when capture cannot continue: a journal file could not be opened,
    /// or a write into an open one failed. The owning process should exit
    /// rather than stay connected while discarding the data it exists to
    /// collect.
    bool Fatal() const {
        return fatal_.load(std::memory_order_acquire);
    }

    std::uint64_t MessagesReceived() const {
        return messages_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t SnapshotsReceived() const {
        return snapshots_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t IncrementalsReceived() const {
        return incrementals_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t ConnectionAttempts() const {
        return connection_attempts_.load(std::memory_order_relaxed);
    }

    std::uint64_t ForcedReconnects() const {
        return forced_reconnects_.load(std::memory_order_relaxed);
    }

  private:
    /// The connection thread: connect, run one session until it ends, back off,
    /// repeat, until stop() or a fatal capture failure.
    void Run();
    /// One connection's whole life. Returns when the session has ended for any
    /// reason; the caller decides whether to retry.
    void RunOneConnection();
    /// Resolves and connects, non-blocking with a poll() deadline, then puts
    /// the socket back into blocking mode with an SO_RCVTIMEO.
    std::expected<int, std::string> ConnectSocket();
    /// Drains every whole message the framer can produce, journaling each one
    /// before classifying it. Returns false when the session must be dropped.
    bool DrainFramedMessages(int fd);
    /// Journals one framed message verbatim, before any parsing. Returns false
    /// on a journal failure that is fatal to capture.
    bool JournalMessage(std::string_view raw);
    /// Sends the scheduled Heartbeat(35=0) if HeartBtInt seconds have passed
    /// since the last outbound byte. Returns false only when the send failed,
    /// which ends the connection. Driven from the top of the read loop rather
    /// than from the recv() timeout branch: what we owe the exchange is a
    /// heartbeat every HeartBtInt of *outbound* silence (exchanges/deribit.md),
    /// which has nothing to do with whether inbound data happens to be flowing.
    bool SendHeartbeatIfDue(int fd);
    /// Writes the whole buffer, tolerating short writes and EINTR. Outbound
    /// only -- these bytes never reach the journal.
    bool SendAll(int fd, std::string_view bytes);
    /// Returns true if shutdown was requested while waiting.
    bool WaitForStop(std::uint64_t millis);
    /// Latches the capture failure and wakes every waiter.
    ///
    /// The mutation is made under `stop_mutex_`, not just the notify: a waiter
    /// evaluates the predicate under that mutex, and a flag flipped outside it
    /// can land in the window between that evaluation and the wait registering
    /// -- the notification is then delivered to nobody and the waiter sleeps out
    /// its whole timeout (up to max_reconnect_wait_ms). The notify itself is
    /// deliberately left outside the lock: by then the new state is already
    /// published, so notifying after unlocking only saves the woken thread from
    /// waking straight onto a mutex this thread still holds.
    void LatchFatal();
    bool Stopping() const {
        return stopping_.load(std::memory_order_acquire);
    }
    /// Log lines tagged with this connection's id.
    void Info(std::string_view message) const;
    void Warn(std::string_view message) const;
    void Error(std::string_view message) const;

    FixClientConfig cfg_;
    /// `cfg_.symbols` comma-joined, for the journal's incarnation marker.
    std::string symbols_text_;
    CaptureSession& capture_;
    FixSession session_;
    StalenessWatchdog watchdog_;

    /// Everything below is touched only by the connection thread.
    fix::Framer framer_;
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
