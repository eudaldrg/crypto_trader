// Two halves of the Deribit FIX client are tested here.
//
// The socket-independent half: the "given this message and this sequence
// verdict, what happens next" decision, and the reconnect backoff curve. Same
// split as capture_session/staleness_watchdog on the Kraken side -- the
// branches worth getting right are the ones a live socket makes hardest to
// exercise (a gap, a peer-initiated Logout, a rejected subscribe on an
// otherwise healthy session), so they are pulled out into pure functions.
//
// The socket half: two paths where acting on that decision is the whole point
// and a pure function cannot prove it happened -- answering a TestRequest with
// a correctly echoed TestReqID, and turning a sequence gap into an actual
// reconnect. Both run the real fix_client, on its real thread, over a real
// loopback TCP socket against a scripted FIX peer (loopback_server below).
// The 45s live testnet run proved neither: no TestRequest arrived in that
// window and no gap occurred (exchanges/deribit.md).
//
// The loopback peer is deliberately single-purpose and lives here rather than
// growing into a reusable fake exchange -- that is Simulation mode
// (decisions/0004) and needs its own design, not an accretion of test helpers.
#include "feed_handler/deribit/deribit_fix_client.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "feed_handler/capture_session.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"

namespace {

using feed_handler::capture_session;
using feed_handler::deribit::classify_inbound;
using feed_handler::deribit::fix_client;
using feed_handler::deribit::fix_client_config;
using feed_handler::deribit::fix_session;
using feed_handler::deribit::inbound_action;
using feed_handler::deribit::inbound_check;
using feed_handler::deribit::inbound_kind;
using feed_handler::deribit::reconnect_delay_ms;
using feed_handler::deribit::sequence_status;
using feed_handler::deribit::session_config;
using feed_handler::fix::field;
using feed_handler::fix::framer;
using feed_handler::fix::parse_message;
namespace msg_type = feed_handler::fix::msg_type;
namespace tag = feed_handler::fix::tag;

/// A throwaway secret that has never been a real Deribit credential.
constexpr std::string_view kTestSecret = "deribit-test-secret-do-not-use-0123456789";
constexpr std::string_view kTestClientId = "test-client";

/// Renders an inbound message the way Deribit would: it is the sender, we are
/// the target.
std::string inbound(std::string_view type, std::uint64_t seq_num,
                    std::span<const field> body = {}) {
    const feed_handler::fix::session_header header{
        .msg_type = type,
        .sender_comp_id = "DERIBITSERVER",
        .target_comp_id = kTestClientId,
        .msg_seq_num = seq_num,
        .sending_time = "20260916-21:30:00.000",
    };
    return feed_handler::fix::build_message(header, body);
}

session_config test_config() {
    return session_config{
        .client_id = std::string(kTestClientId),
        .client_secret = std::string(kTestSecret),
    };
}

/// The verdict for a message that arrived exactly where it was expected, so a
/// test can isolate the message-type branch from the sequence branch.
inbound_check in_sequence(std::uint64_t seq_num) {
    return inbound_check{
        .status = sequence_status::in_sequence,
        .expected = seq_num,
        .received = seq_num,
        .missing = 0,
    };
}

/// How long a loopback step may take before the test calls it a failure.
/// Generous next to the microseconds it actually needs -- the value only has to
/// be smaller than a wedged test and larger than CI scheduling jitter, and
/// nothing waits for it in the passing case.
constexpr int kStepTimeoutMs = 2'000;

/// The client's own timeouts for these tests. recv_timeout_ms doubles as the
/// loop's tick, so it also bounds how long stop() takes; the staleness timeout
/// is far longer than any step here, which is what makes a reconnect in
/// ForcesAReconnect attributable to the gap and nothing else.
constexpr int kLoopbackRecvTimeoutMs = 20;
constexpr std::uint64_t kLoopbackStalenessNs = 5ULL * 1'000'000'000ULL;

/// The sockaddr_in -> sockaddr cast every BSD-socket call needs, in one place.
/// reinterpret_cast rather than the std::bit_cast used elsewhere in this
/// project: bit_cast between pointer types is itself a lint finding
/// (bugprone-bitwise-pointer-cast) and says less about the intent than the cast
/// the socket API was designed around.
::sockaddr* as_sockaddr(::sockaddr_in* address) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<::sockaddr*>(address);
}

/// Owns an fd and closes it exactly once. Movable so an accepted connection can
/// be handed out of accept_one().
class scoped_socket {
  public:
    scoped_socket() = default;
    explicit scoped_socket(int descriptor) : fd_(descriptor) {}
    scoped_socket(const scoped_socket&) = delete;
    scoped_socket& operator=(const scoped_socket&) = delete;
    scoped_socket(scoped_socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    scoped_socket& operator=(scoped_socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~scoped_socket() {
        reset();
    }

    int get() const {
        return fd_;
    }
    bool valid() const {
        return fd_ >= 0;
    }
    void reset() {
        if (fd_ >= 0) {
            ::close(std::exchange(fd_, -1));
        }
    }

  private:
    int fd_ = -1;
};

/// The exchange's end of one accepted connection: whole FIX messages in and
/// out, every read bounded by a deadline so a client that never answers fails
/// the test instead of hanging it.
class peer {
  public:
    /// A default-constructed peer is the "nobody connected" result of
    /// accept_one(); every read and write on one fails.
    peer() = default;
    explicit peer(scoped_socket connection) : fd_(std::move(connection)) {}

    bool connected() const {
        return fd_.valid();
    }

    /// The next whole message the client sent, or nullopt if none arrived in
    /// time (or the connection died, or framing was lost -- all of which are
    /// test failures, none of which are worth telling apart here).
    std::optional<std::string> read_message(int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (const auto framed = framer_.next_message(); framed) {
                // Copied out: the view dies on the next append().
                return std::string(*framed);
            }
            if (!framer_.good()) {
                return std::nullopt;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                return std::nullopt;
            }
            ::pollfd waiting{.fd = fd_.get(), .events = POLLIN, .revents = 0};
            const int ready = ::poll(&waiting, 1, static_cast<int>(remaining));
            if (ready == 0) {
                continue;  // The deadline check at the top decides when to give up.
            }
            if (ready < 0) {
                return std::nullopt;
            }
            std::array<char, 4096> buffer{};
            const ssize_t received = ::recv(fd_.get(), buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                return std::nullopt;
            }
            framer_.append(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
        }
    }

    bool send(std::string_view bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const std::string_view remaining = bytes.substr(sent);
            // MSG_NOSIGNAL for the same reason the client uses it: a peer that
            // went away must be an error return, not a SIGPIPE that takes the
            // whole test binary down.
            const ssize_t written =
                ::send(fd_.get(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(written);
        }
        return true;
    }

  private:
    scoped_socket fd_;
    framer framer_;
};

/// A listening socket on 127.0.0.1, bound to port 0 so the kernel picks a free
/// one -- a hardcoded port would make two of these tests (or two checkouts)
/// collide. Accepting and scripting is the individual test's job; this class
/// deliberately knows nothing about FIX.
class loopback_server {
  public:
    loopback_server() {
        scoped_socket listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener.valid()) {
            return;
        }
        ::sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(0);
        if (::bind(listener.get(), as_sockaddr(&address), sizeof(address)) != 0) {
            return;
        }
        // Backlog room for the reconnect: the client's second connect() can land
        // before the test gets around to accepting it.
        if (::listen(listener.get(), 4) != 0) {
            return;
        }
        ::socklen_t length = sizeof(address);
        if (::getsockname(listener.get(), as_sockaddr(&address), &length) != 0) {
            return;
        }
        port_ = ::ntohs(address.sin_port);
        listener_ = std::move(listener);
    }

    bool listening() const {
        return listener_.valid();
    }
    std::uint16_t port() const {
        return port_;
    }

    /// The next client connection, or a disconnected peer if none arrived in
    /// time.
    peer accept_one(int timeout_ms) {
        ::pollfd waiting{.fd = listener_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, timeout_ms) != 1) {
            return {};
        }
        return peer(scoped_socket(::accept(listener_.get(), nullptr, nullptr)));
    }

  private:
    scoped_socket listener_;
    std::uint16_t port_ = 0;
};

/// Reads one message and checks its MsgType, so a failing test says which
/// message was wrong rather than just "something did not parse".
::testing::AssertionResult expect_next(peer& exchange, std::string_view expected_type,
                                       std::string& raw) {
    const auto message = exchange.read_message(kStepTimeoutMs);
    if (!message) {
        return ::testing::AssertionFailure()
               << "no " << expected_type << " arrived within " << kStepTimeoutMs << "ms";
    }
    const auto parsed = parse_message(*message);
    if (!parsed) {
        return ::testing::AssertionFailure() << "message did not parse: " << parsed.error();
    }
    if (parsed->msg_type() != expected_type) {
        return ::testing::AssertionFailure()
               << "expected MsgType " << expected_type << ", got " << parsed->msg_type();
    }
    raw = *message;
    return ::testing::AssertionSuccess();
}

/// Plays Deribit's side of the handshake: takes the client's Logon, answers
/// with an accepted one, and takes the MarketDataRequest that the acceptance
/// triggers -- so that whatever the client sends next is the thing under test.
::testing::AssertionResult complete_handshake(peer& exchange) {
    std::string raw;
    if (const auto logon = expect_next(exchange, msg_type::logon, raw); !logon) {
        return logon;
    }
    // Deribit answers with its own 35=A carrying EncryptMethod/HeartBtInt
    // (exchanges/deribit.md); MsgSeqNum 1 starts the inbound direction.
    const std::array<field, 2> body = {
        field{.tag = tag::encrypt_method, .value = "0"},
        field{.tag = tag::heart_bt_int, .value = "30"},
    };
    if (!exchange.send(inbound(msg_type::logon, 1, body))) {
        return ::testing::AssertionFailure() << "could not send the accepted Logon";
    }
    return expect_next(exchange, msg_type::market_data_request, raw);
}

}  // namespace

// Tests at namespace scope, after the anonymous namespace closes: cppcheck
// cannot parse TEST macros that follow another definition inside one.

TEST(DeribitInboundDecision, AnAcceptedLogonTriggersTheMarketDataRequest) {
    // Deribit echoes the accepted Logon back as its own 35=A before anything
    // else (experiments/deribit_fix_probe.py), which is the cue to subscribe.
    const std::array<field, 2> body = {
        field{.tag = tag::encrypt_method, .value = "0"},
        field{.tag = tag::heart_bt_int, .value = "30"},
    };
    const std::string raw = inbound(msg_type::logon, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(1));
    EXPECT_EQ(decision.kind, inbound_kind::logon_ack);
    EXPECT_EQ(decision.action, inbound_action::send_market_data_request);
}

TEST(DeribitInboundDecision, ALogonDecisionNeverCarriesCredentialFieldsInItsDetail) {
    // The echoed Logon contains RawData(96) and Password(554). `detail` is what
    // reaches a log line, so nothing derived from those fields may end up in it.
    const std::array<field, 2> body = {
        field{.tag = tag::raw_data, .value = "1700000000000.bm9uY2U="},
        field{.tag = tag::password, .value = "c2VjcmV0LWxvb2tpbmctdmFsdWU="},
    };
    const std::string raw = inbound(msg_type::logon, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    EXPECT_EQ(classify_inbound(*parsed, in_sequence(1)).detail, "");
}

TEST(DeribitInboundDecision, ATestRequestIsAnsweredWithTheEchoedTestReqId) {
    // An unanswered TestRequest ends the session (exchanges/deribit.md), and
    // the answer is only accepted if it echoes this exact id.
    const std::array<field, 1> body = {
        field{.tag = tag::test_req_id, .value = "TEST-4711"},
    };
    const std::string raw = inbound(msg_type::test_request, 7, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(7));
    EXPECT_EQ(decision.kind, inbound_kind::test_request);
    EXPECT_EQ(decision.action, inbound_action::answer_test_request);
    EXPECT_EQ(decision.detail, "TEST-4711");
}

TEST(DeribitInboundDecision, SeparatesSnapshotsFromIncrementalRefreshes) {
    const std::string snapshot = inbound(msg_type::market_data_snapshot_full_refresh, 3);
    const auto parsed_snapshot = parse_message(snapshot);
    ASSERT_TRUE(parsed_snapshot.has_value()) << parsed_snapshot.error();
    const auto snapshot_decision = classify_inbound(*parsed_snapshot, in_sequence(3));
    EXPECT_EQ(snapshot_decision.kind, inbound_kind::market_data_snapshot);
    EXPECT_EQ(snapshot_decision.action, inbound_action::none);

    const std::string incremental = inbound(msg_type::market_data_incremental_refresh, 4);
    const auto parsed_incremental = parse_message(incremental);
    ASSERT_TRUE(parsed_incremental.has_value()) << parsed_incremental.error();
    const auto incremental_decision = classify_inbound(*parsed_incremental, in_sequence(4));
    EXPECT_EQ(incremental_decision.kind, inbound_kind::market_data_incremental);
    EXPECT_EQ(incremental_decision.action, inbound_action::none);
}

TEST(DeribitInboundDecision, ARejectedMarketDataRequestKeepsTheSessionButKeepsTheReason) {
    // The failure mode this exists for: the session stays up, the heartbeats
    // keep flowing, and no market data ever arrives. Reconnecting would only
    // repeat the rejected request, so the answer is a loud log, not a retry.
    const std::array<field, 2> body = {
        field{.tag = tag::md_req_id, .value = "ct-md-1"},
        field{.tag = tag::text, .value = "unknown instrument"},
    };
    const std::string raw = inbound(msg_type::market_data_request_reject, 3, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(3));
    EXPECT_EQ(decision.kind, inbound_kind::market_data_request_reject);
    EXPECT_EQ(decision.action, inbound_action::none);
    EXPECT_EQ(decision.detail, "unknown instrument");
}

TEST(DeribitInboundDecision, APeerInitiatedLogoutEndsTheSession) {
    const std::array<field, 1> body = {
        field{.tag = tag::text, .value = "session expired"},
    };
    const std::string raw = inbound(msg_type::logout, 9, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(9));
    EXPECT_EQ(decision.kind, inbound_kind::logout);
    EXPECT_EQ(decision.action, inbound_action::reconnect);
    EXPECT_EQ(decision.detail, "session expired");
}

TEST(DeribitInboundDecision, ASequenceGapOutranksWhateverTheMessageSays) {
    // decisions/0004: no ResendRequest/SequenceReset gap fill. The messages in
    // the gap are gone, so the book this snapshot would seed cannot be trusted
    // either -- drop the session and start a fresh one.
    fix_session session(test_config());
    const std::string raw = inbound(msg_type::market_data_incremental_refresh, 5);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const inbound_check check = session.on_inbound(*parsed);
    ASSERT_EQ(check.status, sequence_status::gap);
    const auto decision = classify_inbound(*parsed, check);
    EXPECT_EQ(decision.kind, inbound_kind::market_data_incremental);
    EXPECT_EQ(decision.action, inbound_action::reconnect);
    EXPECT_NE(decision.detail.find("gap"), std::string::npos);
}

TEST(DeribitInboundDecision, AnAdministrativeResendIsNotASessionBreak) {
    // PossDupFlag(43)=Y legitimately repeats a sequence number; tearing the
    // session down over one would be self-inflicted.
    fix_session session(test_config());
    const std::array<field, 1> body = {
        field{.tag = tag::poss_dup_flag, .value = "Y"},
    };
    const std::string raw = inbound(msg_type::heartbeat, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const inbound_check check = session.on_inbound(*parsed);
    ASSERT_EQ(check.status, sequence_status::possible_duplicate);
    const auto decision = classify_inbound(*parsed, check);
    EXPECT_EQ(decision.kind, inbound_kind::heartbeat);
    EXPECT_EQ(decision.action, inbound_action::none);
}

TEST(DeribitInboundDecision, AnUnrecognisedMessageTypeIsJournaledAndOtherwiseIgnored) {
    // "unknown" must stay inert: the message has already been journaled by the
    // time this runs, and a type this build does not know is not a reason to
    // drop a working session.
    const std::string raw = inbound("n", 2);  // XMLnonFIX, which this build never asked for.
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(2));
    EXPECT_EQ(decision.kind, inbound_kind::unknown);
    EXPECT_EQ(decision.action, inbound_action::none);
}

TEST(DeribitReconnectBackoff, DoesNotDelayTheFirstConnectionAttempt) {
    EXPECT_EQ(reconnect_delay_ms(0, 1'000, 30'000), 0U);
}

TEST(DeribitReconnectBackoff, DoublesFromTheFloorAndSaturatesAtTheCap) {
    EXPECT_EQ(reconnect_delay_ms(1, 1'000, 30'000), 1'000U);
    EXPECT_EQ(reconnect_delay_ms(2, 1'000, 30'000), 2'000U);
    EXPECT_EQ(reconnect_delay_ms(3, 1'000, 30'000), 4'000U);
    EXPECT_EQ(reconnect_delay_ms(5, 1'000, 30'000), 16'000U);
    EXPECT_EQ(reconnect_delay_ms(6, 1'000, 30'000), 30'000U);
}

TEST(DeribitReconnectBackoff, StaysAtTheCapAcrossALongOutage) {
    // The shift is capped before the clamp so a day-long outage cannot shift a
    // 64-bit value past its width -- that is undefined behaviour, not a large
    // number, and it would be found by an outage rather than by a test run.
    EXPECT_EQ(reconnect_delay_ms(64, 1'000, 30'000), 30'000U);
    EXPECT_EQ(reconnect_delay_ms(100'000, 1'000, 30'000), 30'000U);
}

/// Drives the real fix_client -- real thread, real socket -- against the
/// scripted peer above. Each test gets its own listening port and its own
/// journal directory, and leaves neither behind.
class DeribitFixLoopback : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(server_.listening()) << "could not open a loopback listening socket";
        // Test name and pid: the name keeps the two tests apart, the pid keeps
        // two concurrent runs of this binary (or a leftover dir from a killed
        // one) from writing into each other's journal directory.
        dir_ = std::filesystem::temp_directory_path() /
               ("deribit_fix_loopback_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
                std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    /// Production values (10s connect, 90s staleness, 1-30s backoff) would make
    /// these tests take minutes; these are the same code paths with the clocks
    /// turned down, not different ones.
    fix_client_config loopback_config() const {
        return fix_client_config{
            .host = "127.0.0.1",
            .port = server_.port(),
            .symbol = "BTC-PERPETUAL",
            .md_req_id = "ct-md-loopback",
            .connect_timeout_ms = kStepTimeoutMs,
            .recv_timeout_ms = kLoopbackRecvTimeoutMs,
            .staleness_timeout_ns = kLoopbackStalenessNs,
            .min_reconnect_wait_ms = 10,
            .max_reconnect_wait_ms = 50,
        };
    }

    loopback_server server_;
    std::filesystem::path dir_;
};

TEST_F(DeribitFixLoopback, AnswersATestRequestWithAHeartbeatEchoingItsTestReqId) {
    // An unanswered TestRequest ends the session (exchanges/deribit.md), and an
    // answer carrying the wrong TestReqID is an unanswered one. The decision to
    // reply is unit-tested above; what this adds is that the reply is actually
    // built, sent, and correct on the wire.
    constexpr std::string_view kTestReqId = "TEST-LOOPBACK-4711";

    capture_session capture({.directory = dir_, .exchange = "deribit"});
    fix_client client(test_config(), capture, loopback_config());
    client.start();

    peer exchange = server_.accept_one(kStepTimeoutMs);
    ASSERT_TRUE(exchange.connected()) << "the client never connected";
    ASSERT_TRUE(complete_handshake(exchange));

    const std::array<field, 1> body = {
        field{.tag = tag::test_req_id, .value = std::string(kTestReqId)},
    };
    ASSERT_TRUE(exchange.send(inbound(msg_type::test_request, 2, body)));

    std::string raw;
    ASSERT_TRUE(expect_next(exchange, msg_type::heartbeat, raw));
    const auto answer = parse_message(raw);
    ASSERT_TRUE(answer.has_value()) << answer.error();
    EXPECT_EQ(answer->get(tag::test_req_id).value_or(""), kTestReqId);
    // The reply is a genuine session message, not a bare echo: it carries the
    // next outbound MsgSeqNum (Logon 1, MarketDataRequest 2, Heartbeat 3).
    EXPECT_EQ(answer->get(tag::msg_seq_num).value_or(""), "3");

    client.stop();
    EXPECT_EQ(client.messages_received(), 2U);  // the Logon and the TestRequest
    EXPECT_EQ(client.connection_attempts(), 1U);
}

TEST_F(DeribitFixLoopback, ASequenceGapDropsTheConnectionAndLogsOnAgain) {
    // decisions/0004: no ResendRequest gap fill -- a gap means drop and
    // re-logon. Proving that end to end needs the socket: the pure function
    // only says "reconnect", it cannot show that a second session happened.
    capture_session capture({.directory = dir_, .exchange = "deribit"});
    fix_client client(test_config(), capture, loopback_config());
    client.start();

    peer first = server_.accept_one(kStepTimeoutMs);
    ASSERT_TRUE(first.connected()) << "the client never connected";
    ASSERT_TRUE(complete_handshake(first));
    EXPECT_EQ(client.connection_attempts(), 1U);

    // MsgSeqNum jumps 1 -> 5: three messages the session will never see again.
    ASSERT_TRUE(first.send(inbound(msg_type::market_data_incremental_refresh, 5)));

    // The reconnect itself: a second connection to the same listener, carrying
    // a fresh Logon. connection_attempts_ is incremented before connect(), so
    // by the time this accept() returns the counter has already advanced --
    // there is nothing to poll for and nothing to race with.
    peer second = server_.accept_one(kStepTimeoutMs);
    ASSERT_TRUE(second.connected()) << "the client did not reconnect after the gap";
    std::string raw;
    ASSERT_TRUE(expect_next(second, msg_type::logon, raw));
    EXPECT_GE(client.connection_attempts(), 2U);

    client.stop();
    // Note for whoever reads this next: forced_reconnects() deliberately stays
    // 0 here. It counts only staleness-forced drops; a gap-driven reconnect is
    // not counted anywhere today.
}

TEST_F(DeribitFixLoopback, StampsEveryCapturedFrameWithItsOwnWireShape) {
    // What an order-book sink fed by both exchanges branches on, proven where
    // the frames are real: the session is opened by the client, and the sink
    // sees what the client actually put into it.
    capture_session capture({.directory = dir_, .exchange = "deribit"});
    feed_handler::testing::recording_sink sink;
    capture.add_sink(sink);
    fix_client client(test_config(), capture, loopback_config());
    client.start();

    peer exchange = server_.accept_one(kStepTimeoutMs);
    ASSERT_TRUE(exchange.connected()) << "the client never connected";
    // Returns only once the MarketDataRequest is out, which the client sends
    // strictly after journaling the Logon that triggered it.
    ASSERT_TRUE(complete_handshake(exchange));

    const auto frames = sink.frames();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].source, feed_handler::frame_source::deribit_fix);
    EXPECT_NE(frames[0].payload.find("35=A"), std::string::npos);
    // Sequence 1 is the incarnation marker, which is a journal record rather
    // than a frame, so the first frame a sink sees is 2.
    EXPECT_EQ(frames[0].capture_sequence, 2U);

    const auto incarnations = sink.incarnations();
    ASSERT_EQ(incarnations.size(), 1U);
    EXPECT_EQ(incarnations[0].incarnation, 1U);
    EXPECT_NE(incarnations[0].reason.find("deribit fix"), std::string::npos);

    client.stop();
}
