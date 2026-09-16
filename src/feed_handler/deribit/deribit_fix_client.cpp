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

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// Closes the fd on scope exit. A connection can end at any of a dozen points
/// in run_one_connection(), and leaking one of them would leak the socket too.
class scoped_fd {
  public:
    explicit scoped_fd(int fd) : fd_(fd) {}
    scoped_fd(const scoped_fd&) = delete;
    scoped_fd& operator=(const scoped_fd&) = delete;
    scoped_fd(scoped_fd&&) = delete;
    scoped_fd& operator=(scoped_fd&&) = delete;
    ~scoped_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int get() const {
        return fd_;
    }
    /// Hands ownership back to the caller; the fd is no longer closed here.
    int release() {
        return std::exchange(fd_, -1);
    }

  private:
    int fd_;
};

/// Thread-safe errno rendering: std::system_category() rather than strerror(),
/// whose single static buffer would be a data race the moment a second
/// connection thread exists.
std::string errno_text(int error) {
    return std::system_category().message(error);
}

inbound_kind kind_of(std::string_view msg_type) {
    if (msg_type == fix::msg_type::logon) {
        return inbound_kind::logon_ack;
    }
    if (msg_type == fix::msg_type::heartbeat) {
        return inbound_kind::heartbeat;
    }
    if (msg_type == fix::msg_type::test_request) {
        return inbound_kind::test_request;
    }
    if (msg_type == fix::msg_type::logout) {
        return inbound_kind::logout;
    }
    if (msg_type == fix::msg_type::reject) {
        return inbound_kind::session_reject;
    }
    if (msg_type == fix::msg_type::market_data_snapshot_full_refresh) {
        return inbound_kind::market_data_snapshot;
    }
    if (msg_type == fix::msg_type::market_data_incremental_refresh) {
        return inbound_kind::market_data_incremental;
    }
    if (msg_type == fix::msg_type::market_data_request_reject) {
        return inbound_kind::market_data_request_reject;
    }
    return inbound_kind::unknown;
}

}  // namespace

std::string_view to_string(inbound_kind kind) {
    switch (kind) {
        case inbound_kind::logon_ack:
            return "Logon";
        case inbound_kind::heartbeat:
            return "Heartbeat";
        case inbound_kind::test_request:
            return "TestRequest";
        case inbound_kind::logout:
            return "Logout";
        case inbound_kind::session_reject:
            return "Reject";
        case inbound_kind::market_data_snapshot:
            return "MarketDataSnapshotFullRefresh";
        case inbound_kind::market_data_incremental:
            return "MarketDataIncrementalRefresh";
        case inbound_kind::market_data_request_reject:
            return "MarketDataRequestReject";
        case inbound_kind::unknown:
            break;
    }
    return "unknown";
}

inbound_decision classify_inbound(const fix::parsed_message& message, const inbound_check& check) {
    inbound_decision decision{
        .kind = kind_of(message.msg_type()), .action = inbound_action::none, .detail = {}};

    // The sequence verdict outranks the message type: a gap means the messages
    // in it are gone for good, so whatever this one says, the session is over
    // (decisions/0004 -- detect and reconnect, no ResendRequest gap fill).
    if (check.session_broken()) {
        decision.action = inbound_action::reconnect;
        decision.detail = check.describe();
        return decision;
    }

    switch (decision.kind) {
        case inbound_kind::logon_ack:
            // Deliberately no field values in `detail`: an echoed Logon carries
            // RawData(96)/Password(554), which must never reach a log line.
            decision.action = inbound_action::send_market_data_request;
            break;
        case inbound_kind::test_request:
            decision.action = inbound_action::answer_test_request;
            decision.detail = std::string(message.get(fix::tag::test_req_id).value_or(""));
            break;
        case inbound_kind::logout:
            // The peer ended the session. Reconnecting is the only way back to
            // a data feed, and it is the same path a gap takes.
            decision.action = inbound_action::reconnect;
            decision.detail = std::string(message.get(fix::tag::text).value_or("logged out"));
            break;
        case inbound_kind::session_reject:
        case inbound_kind::market_data_request_reject:
            // Not a reconnect: the session is healthy, it just will not deliver
            // what was asked for. Reconnecting would only repeat the rejected
            // request. The caller's job is to make it loud.
            decision.detail = std::string(message.get(fix::tag::text).value_or(""));
            break;
        case inbound_kind::heartbeat:
        case inbound_kind::market_data_snapshot:
        case inbound_kind::market_data_incremental:
        case inbound_kind::unknown:
            break;
    }
    return decision;
}

std::uint64_t reconnect_delay_ms(std::uint64_t consecutive_failures, std::uint64_t min_ms,
                                 std::uint64_t max_ms) {
    if (consecutive_failures == 0) {
        return 0;
    }
    const std::uint64_t doublings = std::min(consecutive_failures - 1, kMaxBackoffDoublings);
    return std::min(max_ms, min_ms << doublings);
}

fix_client::fix_client(session_config session_cfg, capture_session& capture, fix_client_config cfg)
    : cfg_(std::move(cfg)),
      capture_(capture),
      session_(std::move(session_cfg)),
      watchdog_(cfg_.staleness_timeout_ns) {}

fix_client::~fix_client() {
    stop();
}

void fix_client::start() {
    started_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void fix_client::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void fix_client::run() {
    std::uint64_t consecutive_failures = 0;
    while (!stopping() && !fatal()) {
        const std::uint64_t delay_ms = reconnect_delay_ms(
            consecutive_failures, cfg_.min_reconnect_wait_ms, cfg_.max_reconnect_wait_ms);
        if (delay_ms != 0) {
            log_info("waiting " + std::to_string(delay_ms) + "ms before reconnecting");
            if (wait_for_stop(delay_ms)) {
                return;
            }
        }

        const std::uint64_t before = messages_received();
        run_one_connection();
        // "Did this connection ever deliver anything" is the only usable signal
        // for whether the backoff should keep growing: a connect that succeeds
        // and then immediately dies (a rejected Logon, say) is still a failure,
        // and resetting the counter on connect alone would spin on it.
        consecutive_failures = messages_received() > before ? 0 : consecutive_failures + 1;
    }
}

void fix_client::run_one_connection() {
    connection_attempts_.fetch_add(1, std::memory_order_relaxed);
    const auto connected = connect_socket();
    if (!connected) {
        log_error(connected.error());
        return;
    }
    const scoped_fd socket_fd(*connected);
    log_info("connected to " + cfg_.host + ":" + std::to_string(cfg_.port));

    // Everything that defines a connection incarnation resets together: fresh
    // FIX sequence numbers (Deribit accepts a session restarting at 1 without
    // ResetSeqNumFlag -- exchanges/deribit.md), a fresh framer, and a fresh
    // journal file with its incarnation marker as the first record.
    framer_ = fix::framer{};
    session_.reset_sequence_numbers();
    logged_on_ = false;
    subscribed_ = false;
    last_outbound_ns_ = monotonic_now_ns();

    const auto path = capture_.begin_incarnation("deribit fix " + cfg_.symbol + " connected to " +
                                                 cfg_.host + ":" + std::to_string(cfg_.port));
    if (!path) {
        // Same rule as Kraken: staying connected while unable to capture would
        // silently throw away the data this process exists to collect.
        log_error("cannot start capture: " + path.error());
        fatal_.store(true, std::memory_order_release);
        stop_cv_.notify_all();
        return;
    }
    log_info("incarnation " + std::to_string(capture_.incarnation()) + " started, journaling to " +
             path->string());

    const auto logon = session_.build_logon();
    if (!logon) {
        log_error(logon.error());
        return;
    }
    if (!send_all(socket_fd.get(), *logon)) {
        return;
    }
    log_info("sent Logon");

    watchdog_.note_activity(monotonic_now_ns());
    std::string buffer(kReceiveBufferBytes, '\0');

    while (!stopping() && !fatal()) {
        const ssize_t received = ::recv(socket_fd.get(), buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The SO_RCVTIMEO tick: no data, just the timers.
                if (watchdog_.is_stale(monotonic_now_ns())) {
                    forced_reconnects_.fetch_add(1, std::memory_order_relaxed);
                    watchdog_.disarm();
                    log_warn("forcing reconnect: no inbound message in " +
                             std::to_string(cfg_.staleness_timeout_ns / kNanosPerSecond) + "s");
                    return;
                }
                const std::uint64_t interval_ns =
                    static_cast<std::uint64_t>(session_.config().heartbeat_interval_seconds) *
                    kNanosPerSecond;
                if (logged_on_ && monotonic_now_ns() - last_outbound_ns_ >= interval_ns) {
                    if (!send_all(socket_fd.get(), session_.build_heartbeat())) {
                        return;
                    }
                }
                continue;
            }
            log_warn("recv failed: " + errno_text(errno));
            return;
        }
        if (received == 0) {
            log_warn("deribit closed the connection");
            return;
        }

        watchdog_.note_activity(monotonic_now_ns());
        framer_.append(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
        if (!drain_framed_messages(socket_fd.get())) {
            return;
        }
    }

    if (logged_on_ && stopping()) {
        // Sent from this thread, not from stop(): the socket stays
        // single-threaded, which is the whole point of owning it here.
        send_all(socket_fd.get(), session_.build_logout("shutting down"));
        log_info("sent Logout");
    }
}

bool fix_client::drain_framed_messages(int fd) {
    while (true) {
        const auto framed = framer_.next_message();
        if (!framed) {
            if (!framer_.good()) {
                // Framing is sticky on purpose (fix_message.h): once alignment
                // is lost every following byte is unaligned, and there is no
                // resynchronisation to attempt.
                log_error("framing lost, dropping the connection: " + framer_.error());
                return false;
            }
            return true;
        }

        messages_received_.fetch_add(1, std::memory_order_relaxed);

        // Journal first, classify second (decisions/0004): a message this build
        // does not understand must still reach the file verbatim, so nothing
        // about parsing may be able to cost a record.
        if (!journal_message(*framed)) {
            return false;
        }

        const auto parsed = fix::parse_message(*framed);
        if (!parsed) {
            // A structurally delimited message that fails BodyLength/CheckSum
            // validation means the stream is not what it claims to be; the
            // answer is the same reconnect a gap gets.
            log_error("invalid message, dropping the connection: " + parsed.error());
            return false;
        }

        const inbound_decision decision = classify_inbound(*parsed, session_.on_inbound(*parsed));
        switch (decision.kind) {
            case inbound_kind::logon_ack:
                logged_on_ = true;
                log_info("Logon accepted");
                break;
            case inbound_kind::market_data_snapshot:
                if (snapshots_received_.fetch_add(1, std::memory_order_relaxed) == 0) {
                    log_info("received the first " + std::string(to_string(decision.kind)) +
                             " for " + cfg_.symbol);
                }
                break;
            case inbound_kind::market_data_incremental:
                incrementals_received_.fetch_add(1, std::memory_order_relaxed);
                break;
            case inbound_kind::market_data_request_reject:
                // Loud, for the same reason Kraken's rejected subscribe is: the
                // session stays up and simply never delivers data.
                log_error("deribit rejected the MarketDataRequest: " + decision.detail);
                break;
            case inbound_kind::session_reject:
                log_error("deribit rejected a session message: " + decision.detail);
                break;
            case inbound_kind::heartbeat:
            case inbound_kind::test_request:
            case inbound_kind::logout:
            case inbound_kind::unknown:
                break;
        }

        switch (decision.action) {
            case inbound_action::send_market_data_request:
                if (!subscribed_) {
                    if (!send_all(
                            fd, session_.build_market_data_request(cfg_.md_req_id, cfg_.symbol))) {
                        return false;
                    }
                    subscribed_ = true;
                    log_info("sent MarketDataRequest for " + cfg_.symbol);
                }
                break;
            case inbound_action::answer_test_request:
                if (!send_all(fd, session_.build_heartbeat_response(decision.detail))) {
                    return false;
                }
                break;
            case inbound_action::reconnect:
                log_warn("dropping the session (" + std::string(to_string(decision.kind)) +
                         "): " + decision.detail);
                return false;
            case inbound_action::none:
                break;
        }
    }
}

bool fix_client::journal_message(std::string_view raw) {
    if (capture_.on_wire_message(bytes_of(raw))) {
        return true;
    }
    const std::string_view reason = capture_.error();
    if (reason.empty()) {
        log_error("dropped a message: no journal file open");
        return false;
    }
    log_error("journal write failed: " + std::string(reason));
    fatal_.store(true, std::memory_order_release);
    stop_cv_.notify_all();
    return false;
}

std::expected<int, std::string> fix_client::connect_socket() {
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
            last_error = "socket() failed: " + errno_text(errno);
            continue;
        }
        scoped_fd guard(fd);

        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                last_error = "connect() failed: " + errno_text(errno);
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
                last_error = "poll() failed while connecting: " + errno_text(errno);
                continue;
            }
            // A writable socket does not mean a connected one: the error is
            // only visible through SO_ERROR.
            int so_error = 0;
            ::socklen_t length = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 || so_error != 0) {
                last_error = "connect() failed: " + errno_text(so_error != 0 ? so_error : errno);
                continue;
            }
        }

        // Back to blocking, with a receive timeout: the read loop wants to
        // block, but it also has timers to service, so it needs the block to
        // end on its own.
        const int flags = ::fcntl(fd, F_GETFL, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
            last_error = "cannot clear O_NONBLOCK: " + errno_text(errno);
            continue;
        }
        ::timeval timeout{.tv_sec = cfg_.recv_timeout_ms / 1000,
                          .tv_usec = (cfg_.recv_timeout_ms % 1000) * 1000};
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            last_error = "cannot set SO_RCVTIMEO: " + errno_text(errno);
            continue;
        }
        const int one = 1;
        // Nagle would hold a small Heartbeat back waiting for more data; on a
        // session whose keepalive IS the small message, that is exactly wrong.
        // Not fatal if the kernel refuses it -- it is a latency tweak, not a
        // correctness requirement.
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            log_warn("cannot set TCP_NODELAY: " + errno_text(errno));
        }

        ::freeaddrinfo(resolved);
        // Ownership moves to the caller's own scoped_fd.
        return guard.release();
    }

    ::freeaddrinfo(resolved);
    return std::unexpected("cannot connect to " + cfg_.host + ":" + std::to_string(cfg_.port) +
                           ": " + last_error);
}

bool fix_client::send_all(int fd, std::string_view bytes) {
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
            log_warn("send failed: " + errno_text(errno));
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    last_outbound_ns_ = monotonic_now_ns();
    return true;
}

bool fix_client::wait_for_stop(std::uint64_t millis) {
    std::unique_lock<std::mutex> lock(stop_mutex_);
    return stop_cv_.wait_for(lock, std::chrono::milliseconds(millis),
                             [this] { return stopping() || fatal(); });
}

}  // namespace feed_handler::deribit
