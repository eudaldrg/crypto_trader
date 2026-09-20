#include "feed_handler/deribit/deribit_fix_client.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "feed_handler/logging.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/symbols.h"

namespace feed_handler::deribit {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
/// Caps the doubling in reconnect_delay_ms before it is clamped, so the shift
/// itself can never reach undefined-behaviour territory on a long outage.
constexpr std::uint64_t kMaxBackoffDoublings = 10;
/// One recv() worth of buffer. Deribit's full-book snapshot is far larger than
/// this; the framer is what reassembles a message across reads, so this only
/// sets how many syscalls a big message costs, not what fits.
constexpr std::size_t kReceiveBufferBytes = 64 * 1024;

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// Closes the fd on scope exit. A connection can end at any of a dozen points
/// in run_one_connection(), and leaking one of them would leak the socket too.
class ScopedFd {
  public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&&) = delete;
    ScopedFd& operator=(ScopedFd&&) = delete;
    ~ScopedFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int Get() const {
        return fd_;
    }
    /// Hands ownership back to the caller; the fd is no longer closed here.
    int Release() {
        return std::exchange(fd_, -1);
    }

  private:
    int fd_;
};

/// Closes the capture session on scope exit, whichever way the connection
/// ended. run_one_connection() leaves through a dozen returns -- a gap, a peer
/// Logout, framing loss, a recv() error, the staleness watchdog, shutdown --
/// and the file has to be flushed and closed on every one of them rather than
/// on the next successful connect: that can be a full max_reconnect_wait_ms
/// away, and until then up to a whole 1 MiB write buffer is sitting in this
/// process instead of on disk. Same rule Kraken applies on its Close event, and
/// for the same reason: a closed file is a complete, readable one.
class ScopedCapture {
  public:
    explicit ScopedCapture(CaptureSession& capture) : capture_(&capture) {}
    ScopedCapture(const ScopedCapture&) = delete;
    ScopedCapture& operator=(const ScopedCapture&) = delete;
    ScopedCapture(ScopedCapture&&) = delete;
    ScopedCapture& operator=(ScopedCapture&&) = delete;
    ~ScopedCapture() {
        capture_->Close();
    }

  private:
    CaptureSession* capture_;
};

/// Thread-safe errno rendering: std::system_category() rather than strerror(),
/// whose single static buffer would be a data race the moment a second
/// connection thread exists.
std::string ErrnoText(int error) {
    return std::system_category().message(error);
}

InboundKind KindOf(std::string_view msg_type) {
    if (msg_type == fix::msg_type::kLogon) {
        return InboundKind::kLogonAck;
    }
    if (msg_type == fix::msg_type::kHeartbeat) {
        return InboundKind::kHeartbeat;
    }
    if (msg_type == fix::msg_type::kTestRequest) {
        return InboundKind::kTestRequest;
    }
    if (msg_type == fix::msg_type::kLogout) {
        return InboundKind::kLogout;
    }
    if (msg_type == fix::msg_type::kReject) {
        return InboundKind::kSessionReject;
    }
    if (msg_type == fix::msg_type::kMarketDataSnapshotFullRefresh) {
        return InboundKind::kMarketDataSnapshot;
    }
    if (msg_type == fix::msg_type::kMarketDataIncrementalRefresh) {
        return InboundKind::kMarketDataIncremental;
    }
    if (msg_type == fix::msg_type::kMarketDataRequestReject) {
        return InboundKind::kMarketDataRequestReject;
    }
    return InboundKind::kUnknown;
}

}  // namespace

std::string_view ToString(InboundKind kind) {
    switch (kind) {
        case InboundKind::kLogonAck:
            return "Logon";
        case InboundKind::kHeartbeat:
            return "Heartbeat";
        case InboundKind::kTestRequest:
            return "TestRequest";
        case InboundKind::kLogout:
            return "Logout";
        case InboundKind::kSessionReject:
            return "Reject";
        case InboundKind::kMarketDataSnapshot:
            return "MarketDataSnapshotFullRefresh";
        case InboundKind::kMarketDataIncremental:
            return "MarketDataIncrementalRefresh";
        case InboundKind::kMarketDataRequestReject:
            return "MarketDataRequestReject";
        case InboundKind::kUnknown:
            break;
    }
    return "unknown";
}

InboundDecision ClassifyInbound(const fix::ParsedMessage& message, const InboundCheck& check) {
    InboundDecision decision{
        .kind = KindOf(message.MsgType()), .action = InboundAction::kNone, .detail = {}};

    // The sequence verdict outranks the message type: a gap means the messages
    // in it are gone for good, so whatever this one says, the session is over
    // (decisions/0004 -- detect and reconnect, no ResendRequest gap fill).
    if (check.SessionBroken()) {
        decision.action = InboundAction::kReconnect;
        decision.detail = check.Describe();
        return decision;
    }

    switch (decision.kind) {
        case InboundKind::kLogonAck:
            // Deliberately no field values in `detail`: an echoed Logon carries
            // RawData(96)/Password(554), which must never reach a log line.
            decision.action = InboundAction::kSendMarketDataRequest;
            break;
        case InboundKind::kTestRequest:
            decision.action = InboundAction::kAnswerTestRequest;
            decision.detail = std::string(message.Get(fix::tag::kTestReqId).value_or(""));
            break;
        case InboundKind::kLogout:
            // The peer ended the session. Reconnecting is the only way back to
            // a data feed, and it is the same path a gap takes.
            decision.action = InboundAction::kReconnect;
            decision.detail = std::string(message.Get(fix::tag::kText).value_or("logged out"));
            break;
        case InboundKind::kSessionReject:
        case InboundKind::kMarketDataRequestReject:
            // Not a reconnect: the session is healthy, it just will not deliver
            // what was asked for. Reconnecting would only repeat the rejected
            // request. The caller's job is to make it loud.
            decision.detail = std::string(message.Get(fix::tag::kText).value_or(""));
            break;
        case InboundKind::kHeartbeat:
        case InboundKind::kMarketDataSnapshot:
        case InboundKind::kMarketDataIncremental:
        case InboundKind::kUnknown:
            break;
    }
    return decision;
}

std::uint64_t ReconnectDelayMs(std::uint64_t consecutive_failures, std::uint64_t min_ms,
                               std::uint64_t max_ms) {
    if (consecutive_failures == 0) {
        return 0;
    }
    const std::uint64_t doublings = std::min(consecutive_failures - 1, kMaxBackoffDoublings);
    return std::min(max_ms, min_ms << doublings);
}

FixClient::FixClient(SessionConfig session_cfg, CaptureSession& capture, FixClientConfig cfg)
    : cfg_(std::move(cfg)),
      log_(cfg_.id),
      capture_(capture),
      session_(std::move(session_cfg)),
      watchdog_(cfg_.staleness_timeout_ns) {}

FixClient::~FixClient() {
    Stop();
}

void FixClient::Start() {
    started_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
}

void FixClient::RequestStop() {
    // Joining stays in Join(): the thread it waits for needs the signal's mutex
    // to notice the request.
    stop_signal_.RequestStop();
}

void FixClient::Join() {
    RequestStop();
    if (joined_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FixClient::Stop() {
    RequestStop();
    Join();
}

void FixClient::Run() {
    std::uint64_t consecutive_failures = 0;
    while (!stop_signal_.Stopping()) {
        const std::uint64_t delay_ms = ReconnectDelayMs(
            consecutive_failures, cfg_.min_reconnect_wait_ms, cfg_.max_reconnect_wait_ms);
        if (delay_ms != 0) {
            log_.Info("waiting " + std::to_string(delay_ms) + "ms before reconnecting");
            if (stop_signal_.WaitFor(std::chrono::milliseconds(delay_ms))) {
                return;
            }
        }

        const std::uint64_t before = MessagesReceived();
        RunOneConnection();
        // "Did this connection ever deliver anything" is the only usable signal
        // for whether the backoff should keep growing: a connect that succeeds
        // and then immediately dies (a rejected Logon, say) is still a failure,
        // and resetting the counter on connect alone would spin on it.
        consecutive_failures = MessagesReceived() > before ? 0 : consecutive_failures + 1;
    }
}

void FixClient::RunOneConnection() {
    connection_attempts_.fetch_add(1, std::memory_order_relaxed);
    const auto connected = ConnectSocket();
    if (!connected) {
        log_.Error(connected.error());
        return;
    }
    const ScopedFd socket_fd(*connected);
    // Declared before the incarnation it closes, so no exit below can skip it.
    // Closing a session with nothing open is a no-op, so it is also harmless if
    // begin_incarnation() itself fails.
    const ScopedCapture capture_guard(capture_);
    log_.Info("connected to " + cfg_.host + ":" + std::to_string(cfg_.port));

    // Everything that defines a connection incarnation resets together: fresh
    // FIX sequence numbers (Deribit accepts a session restarting at 1 without
    // ResetSeqNumFlag -- exchanges/deribit.md), a fresh framer, and a fresh
    // journal file with its incarnation marker as the first record.
    framer_ = fix::Framer{};
    session_.ResetSequenceNumbers();
    logged_on_ = false;
    subscribed_ = false;
    last_outbound_ns_ = MonotonicNowNs();

    const auto path =
        capture_.BeginIncarnation("deribit fix " + JoinSymbols(cfg_.symbols) + " connected to " +
                                      cfg_.host + ":" + std::to_string(cfg_.port),
                                  kWireSource);
    if (!path) {
        // Same rule as Kraken: staying connected while unable to capture would
        // silently throw away the data this process exists to collect.
        log_.Error("cannot start capture: " + path.error());
        stop_signal_.LatchFatal();
        return;
    }
    log_.Info("incarnation " + std::to_string(capture_.Incarnation()) + " started, journaling to " +
              path->string());

    const auto logon = session_.BuildLogon();
    if (!logon) {
        log_.Error(logon.error());
        return;
    }
    if (!SendAll(socket_fd.Get(), *logon)) {
        return;
    }
    log_.Info("sent Logon");

    watchdog_.NoteActivity(MonotonicNowNs());
    std::string buffer(kReceiveBufferBytes, '\0');

    while (!stop_signal_.Stopping()) {
        // Before the recv(), not inside its timeout branch: the Heartbeat is
        // owed on outbound silence (exchanges/deribit.md), and on a busy feed
        // recv() keeps returning data promptly, so a check that only ran when
        // the receive timed out would never run at all on exactly the session
        // that is carrying real market data. The check itself is a monotonic
        // clock read and a comparison, so running it every iteration is free.
        if (!SendHeartbeatIfDue(socket_fd.Get())) {
            return;
        }

        const ssize_t received = ::recv(socket_fd.Get(), buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The SO_RCVTIMEO tick: no data, just the timers.
                if (watchdog_.IsStale(MonotonicNowNs())) {
                    forced_reconnects_.fetch_add(1, std::memory_order_relaxed);
                    watchdog_.Disarm();
                    log_.Warn("forcing reconnect: no inbound message in " +
                              std::to_string(cfg_.staleness_timeout_ns / kNanosPerSecond) + "s");
                    return;
                }
                continue;
            }
            log_.Warn("recv failed: " + ErrnoText(errno));
            return;
        }
        if (received == 0) {
            log_.Warn("deribit closed the connection");
            return;
        }

        watchdog_.NoteActivity(MonotonicNowNs());
        framer_.Append(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
        if (!DrainFramedMessages(socket_fd.Get())) {
            return;
        }
    }

    if (logged_on_ && stop_signal_.StopRequested()) {
        // Sent from this thread, not from stop(): the socket stays
        // single-threaded, which is the whole point of owning it here.
        SendAll(socket_fd.Get(), session_.BuildLogout("shutting down"));
        log_.Info("sent Logout");
    }
}

bool FixClient::DrainFramedMessages(int fd) {
    while (true) {
        const auto framed = framer_.NextMessage();
        if (!framed) {
            if (!framer_.Good()) {
                // Framing is sticky on purpose (fix_message.h): once alignment
                // is lost every following byte is unaligned, and there is no
                // resynchronisation to attempt.
                log_.Error("framing lost, dropping the connection: " + framer_.Error());
                return false;
            }
            return true;
        }

        messages_received_.fetch_add(1, std::memory_order_relaxed);

        // Journal first, classify second (decisions/0004): a message this build
        // does not understand must still reach the file verbatim, so nothing
        // about parsing may be able to cost a record.
        if (!JournalMessage(*framed)) {
            return false;
        }

        const auto parsed = fix::ParseMessage(*framed);
        if (!parsed) {
            // A structurally delimited message that fails BodyLength/CheckSum
            // validation means the stream is not what it claims to be; the
            // answer is the same reconnect a gap gets.
            log_.Error("invalid message, dropping the connection: " + parsed.error());
            return false;
        }

        const InboundDecision decision = ClassifyInbound(*parsed, session_.OnInbound(*parsed));
        switch (decision.kind) {
            case InboundKind::kLogonAck:
                logged_on_ = true;
                log_.Info("Logon accepted");
                break;
            case InboundKind::kMarketDataSnapshot:
                // One 35=W per requested symbol per (re)connect, so logging each
                // is a per-symbol subscription check the way Kraken's per-symbol
                // acks are.
                snapshots_received_.fetch_add(1, std::memory_order_relaxed);
                log_.Info("received a " + std::string(ToString(decision.kind)) + " for " +
                          std::string(parsed->Get(fix::tag::kSymbol).value_or("<no symbol>")));
                break;
            case InboundKind::kMarketDataIncremental:
                incrementals_received_.fetch_add(1, std::memory_order_relaxed);
                break;
            case InboundKind::kMarketDataRequestReject:
                // Loud, for the same reason Kraken's rejected subscribe is: the
                // session stays up and simply never delivers data.
                log_.Error("deribit rejected the MarketDataRequest: " + decision.detail);
                break;
            case InboundKind::kSessionReject:
                log_.Error("deribit rejected a session message: " + decision.detail);
                break;
            case InboundKind::kHeartbeat:
            case InboundKind::kTestRequest:
            case InboundKind::kLogout:
            case InboundKind::kUnknown:
                break;
        }

        switch (decision.action) {
            case InboundAction::kSendMarketDataRequest:
                if (!subscribed_) {
                    if (!SendAll(fd,
                                 session_.BuildMarketDataRequest(cfg_.md_req_id, cfg_.symbols))) {
                        return false;
                    }
                    subscribed_ = true;
                    log_.Info("sent MarketDataRequest for " + std::to_string(cfg_.symbols.size()) +
                              " symbol(s): " + JoinSymbols(cfg_.symbols));
                }
                break;
            case InboundAction::kAnswerTestRequest:
                if (!SendAll(fd, session_.BuildHeartbeatResponse(decision.detail))) {
                    return false;
                }
                break;
            case InboundAction::kReconnect:
                log_.Warn("dropping the session (" + std::string(ToString(decision.kind)) +
                          "): " + decision.detail);
                return false;
            case InboundAction::kNone:
                break;
        }
    }
}

bool FixClient::JournalMessage(std::string_view raw) {
    if (capture_.OnWireMessage(BytesOf(raw), kWireSource)) {
        return true;
    }
    const std::string_view reason = capture_.Error();
    if (reason.empty()) {
        log_.Error("dropped a message: no journal file open");
        return false;
    }
    log_.Error("journal write failed: " + std::string(reason));
    stop_signal_.LatchFatal();
    return false;
}

bool FixClient::SendHeartbeatIfDue(int fd) {
    // Nothing is owed before the Logon is accepted: the session that the
    // interval belongs to does not exist yet.
    if (!logged_on_ || session_.Config().heartbeat_interval_seconds <= 0) {
        // HeartBtInt(108)=0 is FIX for "no heartbeats", and sending on a zero
        // interval would be one Heartbeat per loop tick rather than none.
        return true;
    }
    const std::uint64_t interval_ns =
        static_cast<std::uint64_t>(session_.Config().heartbeat_interval_seconds) * kNanosPerSecond;
    if (MonotonicNowNs() - last_outbound_ns_ < interval_ns) {
        return true;
    }
    // send_all() restamps last_outbound_ns_, so answering a TestRequest or
    // sending a subscribe postpones the scheduled Heartbeat exactly as the
    // "outbound silence" rule says it should.
    return SendAll(fd, session_.BuildHeartbeat());
}

std::expected<int, std::string> FixClient::ConnectSocket() {
    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ::addrinfo* resolved = nullptr;
    const int rc =
        ::getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &resolved);
    if (rc != 0) {
        return std::unexpected("cannot resolve " + cfg_.host + ": " + ::gai_strerror(rc));
    }

    std::string last_error = "no address for " + cfg_.host;
    for (const ::addrinfo* candidate = resolved; candidate != nullptr;
         candidate = candidate->ai_next) {
        // SOCK_NONBLOCK at creation rather than an extra fcntl: connect() must
        // not park this thread for the kernel's own SYN timeout, which is
        // minutes and would make shutdown look hung.
        const int fd = ::socket(candidate->ai_family, candidate->ai_socktype | SOCK_NONBLOCK,
                                candidate->ai_protocol);
        if (fd < 0) {
            last_error = "socket() failed: " + ErrnoText(errno);
            continue;
        }
        ScopedFd guard(fd);

        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                last_error = "connect() failed: " + ErrnoText(errno);
                continue;
            }
            ::pollfd waiting{.fd = fd, .events = POLLOUT, .revents = 0};
            const int ready = ::poll(&waiting, 1, cfg_.connect_timeout_ms);
            if (ready == 0) {
                last_error =
                    "connect() timed out after " + std::to_string(cfg_.connect_timeout_ms) + "ms";
                continue;
            }
            if (ready < 0) {
                last_error = "poll() failed while connecting: " + ErrnoText(errno);
                continue;
            }
            // A writable socket does not mean a connected one: the error is
            // only visible through SO_ERROR.
            int so_error = 0;
            ::socklen_t length = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 || so_error != 0) {
                last_error = "connect() failed: " + ErrnoText(so_error != 0 ? so_error : errno);
                continue;
            }
        }

        // Back to blocking, with a receive timeout: the read loop wants to
        // block, but it also has timers to service, so it needs the block to
        // end on its own.
        const int flags = ::fcntl(fd, F_GETFL, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
            last_error = "cannot clear O_NONBLOCK: " + ErrnoText(errno);
            continue;
        }
        ::timeval timeout{.tv_sec = cfg_.recv_timeout_ms / 1000,
                          .tv_usec = (cfg_.recv_timeout_ms % 1000) * 1000};
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            last_error = "cannot set SO_RCVTIMEO: " + ErrnoText(errno);
            continue;
        }
        const int one = 1;
        // Nagle would hold a small Heartbeat back waiting for more data; on a
        // session whose keepalive IS the small message, that is exactly wrong.
        // Not fatal if the kernel refuses it -- it is a latency tweak, not a
        // correctness requirement.
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            log_.Warn("cannot set TCP_NODELAY: " + ErrnoText(errno));
        }

        ::freeaddrinfo(resolved);
        // Ownership moves to the caller's own scoped_fd.
        return guard.Release();
    }

    ::freeaddrinfo(resolved);
    return std::unexpected("cannot connect to " + cfg_.host + ":" + std::to_string(cfg_.port) +
                           ": " + last_error);
}

bool FixClient::SendAll(int fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        // MSG_NOSIGNAL rather than an ignored SIGPIPE: writing to a peer that
        // has gone away must be an error return here, not a process-wide signal
        // disposition this library imposes on whoever links it.
        const ssize_t written = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_.Warn("send failed: " + ErrnoText(errno));
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    last_outbound_ns_ = MonotonicNowNs();
    return true;
}

}  // namespace feed_handler::deribit
