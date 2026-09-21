// Two halves of the Deribit FIX client are tested here.
//
// The socket-independent half: the "given this message and this sequence
// verdict, what happens next" decision, and the reconnect backoff curve. Same
// split as CaptureSession/StalenessWatchdog on the Kraken side -- the
// branches worth getting right are the ones a live socket makes hardest to
// exercise (a gap, a peer-initiated Logout, a rejected subscribe on an
// otherwise healthy session), so they are pulled out into pure functions.
//
// The socket half: the paths where acting on that decision is the whole point
// and a pure function cannot prove it happened -- answering a TestRequest with
// a correctly echoed TestReqID, turning a sequence gap into an actual
// reconnect, stamping captured frames with this client's own wire shape, and
// leaving each connect's journal complete on disk however the connection
// ended. All run the real FixClient, on its real thread, over a real loopback
// TCP socket against a scripted FIX peer (loopback_server below). The 45s live
// testnet run proved none of them: no TestRequest arrived in that window and no
// gap occurred (exchanges/deribit.md).
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
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "feed_handler/capture_session.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureSession;
using feed_handler::deribit::ClassifyInbound;
using feed_handler::deribit::FixClient;
using feed_handler::deribit::FixClientConfig;
using feed_handler::deribit::FixSession;
using feed_handler::deribit::InboundAction;
using feed_handler::deribit::InboundCheck;
using feed_handler::deribit::InboundKind;
using feed_handler::deribit::ReconnectDelayMs;
using feed_handler::deribit::SequenceStatus;
using feed_handler::deribit::SessionConfig;
using feed_handler::fix::Field;
using feed_handler::fix::Framer;
using feed_handler::fix::ParseMessage;
namespace msg_type = feed_handler::fix::msg_type;
namespace tag = feed_handler::fix::tag;

/// A throwaway secret that has never been a real Deribit credential.
constexpr std::string_view kTestSecret = "deribit-test-secret-do-not-use-0123456789";
constexpr std::string_view kTestClientId = "test-client";

/// Renders an inbound message the way Deribit would: it is the sender, we are
/// the target.
std::string Inbound(std::string_view type, std::uint64_t seq_num,
                    std::span<const Field> body = {}) {
    const feed_handler::fix::SessionHeader header{
        .msg_type = type,
        .sender_comp_id = "DERIBITSERVER",
        .target_comp_id = kTestClientId,
        .msg_seq_num = seq_num,
        .sending_time = "20260916-21:30:00.000",
    };
    return feed_handler::fix::BuildMessage(header, body);
}

SessionConfig TestConfig() {
    return SessionConfig{
        .client_id = std::string(kTestClientId),
        .client_secret = std::string(kTestSecret),
    };
}

/// The verdict for a message that arrived exactly where it was expected, so a
/// test can isolate the message-type branch from the sequence branch.
InboundCheck InSequence(std::uint64_t seq_num) {
    return InboundCheck{
        .status = SequenceStatus::kInSequence,
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

/// The busy-connection Heartbeat test's clocks. One inbound message every
/// kTrickleIntervalMs, against a receive timeout a hundred times longer, is what
/// makes the recv() timeout branch unreachable for that test's duration: if a
/// Heartbeat appears, the loop's own schedule is the only thing that can have
/// sent it. kHeartbeatDeadlineMs is slack around the 1s interval, not a value
/// anything waits for when the client behaves.
constexpr int kLoopbackHeartbeatSeconds = 1;
constexpr int kTrickleIntervalMs = 5;
constexpr int kBusyRecvTimeoutMs = 500;
constexpr int kHeartbeatDeadlineMs = 4'000;

/// The shutdown-promptness test's clocks: a backoff long enough that sleeping
/// through it is unmistakable, a bound far below it, and enough attempts to give
/// the stop()-versus-wait race a fair number of chances to be lost.
constexpr std::uint64_t kStuckBackoffMs = 10'000;
constexpr int kPromptStopMs = 1'000;
constexpr int kStopRaceAttempts = 20;

/// The sockaddr_in -> sockaddr cast every BSD-socket call needs, in one place.
/// reinterpret_cast rather than the std::bit_cast used elsewhere in this
/// project: bit_cast between pointer types is itself a lint finding
/// (bugprone-bitwise-pointer-cast) and says less about the intent than the cast
/// the socket API was designed around.
::sockaddr* AsSockaddr(::sockaddr_in* address) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<::sockaddr*>(address);
}

/// Owns an fd and closes it exactly once. Movable so an accepted connection can
/// be handed out of accept_one().
class ScopedSocket {
  public:
    ScopedSocket() = default;
    explicit ScopedSocket(int descriptor) : fd_(descriptor) {}
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
    ScopedSocket(ScopedSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    ScopedSocket& operator=(ScopedSocket&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~ScopedSocket() {
        Reset();
    }

    int Get() const {
        return fd_;
    }
    bool Valid() const {
        return fd_ >= 0;
    }
    void Reset() {
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
class Peer {
  public:
    /// A default-constructed peer is the "nobody connected" result of
    /// accept_one(); every read and write on one fails.
    Peer() = default;
    explicit Peer(ScopedSocket connection) : fd_(std::move(connection)) {}

    bool Connected() const {
        return fd_.Valid();
    }

    /// The next whole message the client sent, or nullopt if none arrived in
    /// time (or the connection died, or framing was lost -- all of which are
    /// test failures, none of which are worth telling apart here).
    std::optional<std::string> ReadMessage(int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (const auto framed = framer_.NextMessage(); framed) {
                // Copied out: the view dies on the next append().
                return std::string(*framed);
            }
            if (!framer_.Good()) {
                return std::nullopt;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                return std::nullopt;
            }
            ::pollfd waiting{.fd = fd_.Get(), .events = POLLIN, .revents = 0};
            const int ready = ::poll(&waiting, 1, static_cast<int>(remaining));
            if (ready == 0) {
                continue;  // The deadline check at the top decides when to give up.
            }
            if (ready < 0) {
                return std::nullopt;
            }
            std::array<char, 4096> buffer{};
            const ssize_t received = ::recv(fd_.Get(), buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                return std::nullopt;
            }
            framer_.Append(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
        }
    }

    bool Send(std::string_view bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const std::string_view remaining = bytes.substr(sent);
            // MSG_NOSIGNAL for the same reason the client uses it: a peer that
            // went away must be an error return, not a SIGPIPE that takes the
            // whole test binary down.
            const ssize_t written =
                ::send(fd_.Get(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(written);
        }
        return true;
    }

  private:
    ScopedSocket fd_;
    Framer framer_;
};

/// A listening socket on 127.0.0.1, bound to port 0 so the kernel picks a free
/// one -- a hardcoded port would make two of these tests (or two checkouts)
/// collide. Accepting and scripting is the individual test's job; this class
/// deliberately knows nothing about FIX.
class LoopbackServer {
  public:
    LoopbackServer() {
        ScopedSocket listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener.Valid()) {
            return;
        }
        ::sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(0);
        if (::bind(listener.Get(), AsSockaddr(&address), sizeof(address)) != 0) {
            return;
        }
        // Backlog room for the reconnect: the client's second connect() can land
        // before the test gets around to accepting it.
        if (::listen(listener.Get(), 4) != 0) {
            return;
        }
        ::socklen_t length = sizeof(address);
        if (::getsockname(listener.Get(), AsSockaddr(&address), &length) != 0) {
            return;
        }
        port_ = ::ntohs(address.sin_port);
        listener_ = std::move(listener);
    }

    bool Listening() const {
        return listener_.Valid();
    }
    std::uint16_t Port() const {
        return port_;
    }

    /// Stops accepting, so the client's next connect() is refused instead of
    /// queued. What that buys a test: the client leaves run_one_connection()
    /// and stays out of it, rather than immediately opening the next
    /// connect -- which would close the previous one anyway and hide
    /// whether the path under test closed it.
    void StopListening() {
        listener_.Reset();
    }

    /// The next client connection, or a disconnected peer if none arrived in
    /// time.
    Peer AcceptOne(int timeout_ms) {
        ::pollfd waiting{.fd = listener_.Get(), .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, timeout_ms) != 1) {
            return {};
        }
        return Peer(ScopedSocket(::accept(listener_.Get(), nullptr, nullptr)));
    }

  private:
    ScopedSocket listener_;
    std::uint16_t port_ = 0;
};

/// Reads one message and checks its MsgType, so a failing test says which
/// message was wrong rather than just "something did not parse".
::testing::AssertionResult ExpectNext(Peer& exchange, std::string_view expected_type,
                                      std::string& raw) {
    const auto message = exchange.ReadMessage(kStepTimeoutMs);
    if (!message) {
        return ::testing::AssertionFailure()
               << "no " << expected_type << " arrived within " << kStepTimeoutMs << "ms";
    }
    const auto parsed = ParseMessage(*message);
    if (!parsed) {
        return ::testing::AssertionFailure() << "message did not parse: " << parsed.error();
    }
    if (parsed->MsgType() != expected_type) {
        return ::testing::AssertionFailure()
               << "expected MsgType " << expected_type << ", got " << parsed->MsgType();
    }
    raw = *message;
    return ::testing::AssertionSuccess();
}

/// Plays Deribit's side of the handshake: takes the client's Logon, answers
/// with an accepted one, and takes the MarketDataRequest that the acceptance
/// triggers -- so that whatever the client sends next is the thing under test.
::testing::AssertionResult CompleteHandshake(Peer& exchange) {
    std::string raw;
    if (const auto logon = ExpectNext(exchange, msg_type::kLogon, raw); !logon) {
        return logon;
    }
    // Deribit answers with its own 35=A carrying EncryptMethod/HeartBtInt
    // (exchanges/deribit.md); MsgSeqNum 1 starts the inbound direction.
    const std::array<Field, 2> body = {
        Field{.tag = tag::kEncryptMethod, .value = "0"},
        Field{.tag = tag::kHeartBtInt, .value = "30"},
    };
    if (!exchange.Send(Inbound(msg_type::kLogon, 1, body))) {
        return ::testing::AssertionFailure() << "could not send the accepted Logon";
    }
    return ExpectNext(exchange, msg_type::kMarketDataRequest, raw);
}

/// The journal file one connect of `exchange` wrote into `directory`, found
/// by the ordinal the file name carries (capture_session.h). Empty if the
/// connect never opened a file.
std::filesystem::path JournalOf(const std::filesystem::path& directory, std::uint64_t connect_id) {
    // Only the exchange and ordinal are predictable; the rest of the name is a
    // UTC timestamp taken when the file was created (capture_session.h).
    const std::string prefix = std::format("deribit-{:06}-", connect_id);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().filename().string().starts_with(prefix)) {
            return entry.path();
        }
    }
    return {};
}

/// Waits for one connect's journal file to be complete on disk: present,
/// readable, holding exactly `expected_records` records and ending at clean
/// EOF rather than on a torn tail.
///
/// That is the observable form of "the capture session was closed": records sit
/// in a 1 MiB userspace buffer until something flushes it, so a file that reads
/// back in full is a file that was flushed and closed. Polled because the
/// closing happens on the client's own connection thread.
::testing::AssertionResult ExpectClosedJournal(const std::filesystem::path& directory,
                                               std::uint64_t connect_id,
                                               std::uint64_t expected_records) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStepTimeoutMs);
    std::string last_problem = "no journal file for connect_id " + std::to_string(connect_id);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::filesystem::path path = JournalOf(directory, connect_id);
        if (!path.empty()) {
            auto reader = feed_handler::JournalReader::Open(path);
            if (!reader) {
                last_problem = "journal unreadable: " + reader.error();
            } else {
                std::uint64_t records = 0;
                while (reader->Next().has_value()) {
                    ++records;
                }
                if (records == expected_records && !reader->StoppedEarly()) {
                    return ::testing::AssertionSuccess();
                }
                last_problem = "journal holds " + std::to_string(records) + " of " +
                               std::to_string(expected_records) + " records" +
                               (reader->StoppedEarly()
                                    ? ", stopped early: " + std::string(reader->StopReason())
                                    : "");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ::testing::AssertionFailure()
           << "connect_id " << connect_id << "'s journal was never flushed and closed ("
           << last_problem << ")";
}

}  // namespace

// Tests at namespace scope, after the anonymous namespace closes: cppcheck
// cannot parse TEST macros that follow another definition inside one.

TEST(DeribitInboundDecision, AnAcceptedLogonTriggersTheMarketDataRequest) {
    // Deribit echoes the accepted Logon back as its own 35=A before anything
    // else (experiments/deribit_fix_probe.py), which is the cue to subscribe.
    const std::array<Field, 2> body = {
        Field{.tag = tag::kEncryptMethod, .value = "0"},
        Field{.tag = tag::kHeartBtInt, .value = "30"},
    };
    const std::string raw = Inbound(msg_type::kLogon, 1, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = ClassifyInbound(*parsed, InSequence(1));
    EXPECT_EQ(decision.kind, InboundKind::kLogonAck);
    EXPECT_EQ(decision.action, InboundAction::kSendMarketDataRequest);
}

TEST(DeribitInboundDecision, ALogonDecisionNeverCarriesCredentialFieldsInItsDetail) {
    // The echoed Logon contains RawData(96) and Password(554). `detail` is what
    // reaches a log line, so nothing derived from those fields may end up in it.
    const std::array<Field, 2> body = {
        Field{.tag = tag::kRawData, .value = "1700000000000.bm9uY2U="},
        Field{.tag = tag::kPassword, .value = "c2VjcmV0LWxvb2tpbmctdmFsdWU="},
    };
    const std::string raw = Inbound(msg_type::kLogon, 1, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    EXPECT_EQ(ClassifyInbound(*parsed, InSequence(1)).detail, "");
}

TEST(DeribitInboundDecision, ATestRequestIsAnsweredWithTheEchoedTestReqId) {
    // An unanswered TestRequest ends the session (exchanges/deribit.md), and
    // the answer is only accepted if it echoes this exact id.
    const std::array<Field, 1> body = {
        Field{.tag = tag::kTestReqId, .value = "TEST-4711"},
    };
    const std::string raw = Inbound(msg_type::kTestRequest, 7, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = ClassifyInbound(*parsed, InSequence(7));
    EXPECT_EQ(decision.kind, InboundKind::kTestRequest);
    EXPECT_EQ(decision.action, InboundAction::kAnswerTestRequest);
    EXPECT_EQ(decision.detail, "TEST-4711");
}

TEST(DeribitInboundDecision, SeparatesSnapshotsFromIncrementalRefreshes) {
    const std::string snapshot = Inbound(msg_type::kMarketDataSnapshotFullRefresh, 3);
    const auto parsed_snapshot = ParseMessage(snapshot);
    ASSERT_TRUE(parsed_snapshot.has_value()) << parsed_snapshot.error();
    const auto snapshot_decision = ClassifyInbound(*parsed_snapshot, InSequence(3));
    EXPECT_EQ(snapshot_decision.kind, InboundKind::kMarketDataSnapshot);
    EXPECT_EQ(snapshot_decision.action, InboundAction::kNone);

    const std::string incremental = Inbound(msg_type::kMarketDataIncrementalRefresh, 4);
    const auto parsed_incremental = ParseMessage(incremental);
    ASSERT_TRUE(parsed_incremental.has_value()) << parsed_incremental.error();
    const auto incremental_decision = ClassifyInbound(*parsed_incremental, InSequence(4));
    EXPECT_EQ(incremental_decision.kind, InboundKind::kMarketDataIncremental);
    EXPECT_EQ(incremental_decision.action, InboundAction::kNone);
}

TEST(DeribitInboundDecision, ARejectedMarketDataRequestKeepsTheSessionButKeepsTheReason) {
    // The failure mode this exists for: the session stays up, the heartbeats
    // keep flowing, and no market data ever arrives. Reconnecting would only
    // repeat the rejected request, so the answer is a loud log, not a retry.
    const std::array<Field, 2> body = {
        Field{.tag = tag::kMdReqId, .value = "ct-md-1"},
        Field{.tag = tag::kText, .value = "unknown instrument"},
    };
    const std::string raw = Inbound(msg_type::kMarketDataRequestReject, 3, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = ClassifyInbound(*parsed, InSequence(3));
    EXPECT_EQ(decision.kind, InboundKind::kMarketDataRequestReject);
    EXPECT_EQ(decision.action, InboundAction::kNone);
    EXPECT_EQ(decision.detail, "unknown instrument");
}

TEST(DeribitInboundDecision, APeerInitiatedLogoutEndsTheSession) {
    const std::array<Field, 1> body = {
        Field{.tag = tag::kText, .value = "session expired"},
    };
    const std::string raw = Inbound(msg_type::kLogout, 9, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = ClassifyInbound(*parsed, InSequence(9));
    EXPECT_EQ(decision.kind, InboundKind::kLogout);
    EXPECT_EQ(decision.action, InboundAction::kReconnect);
    EXPECT_EQ(decision.detail, "session expired");
}

TEST(DeribitInboundDecision, ASequenceGapOutranksWhateverTheMessageSays) {
    // decisions/0004: no ResendRequest/SequenceReset gap fill. The messages in
    // the gap are gone, so the book this snapshot would seed cannot be trusted
    // either -- drop the session and start a fresh one.
    FixSession session(TestConfig());
    const std::string raw = Inbound(msg_type::kMarketDataIncrementalRefresh, 5);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const InboundCheck check = session.OnInbound(*parsed);
    ASSERT_EQ(check.status, SequenceStatus::kGap);
    const auto decision = ClassifyInbound(*parsed, check);
    EXPECT_EQ(decision.kind, InboundKind::kMarketDataIncremental);
    EXPECT_EQ(decision.action, InboundAction::kReconnect);
    EXPECT_NE(decision.detail.find("gap"), std::string::npos);
}

TEST(DeribitInboundDecision, AnAdministrativeResendIsNotASessionBreak) {
    // PossDupFlag(43)=Y legitimately repeats a sequence number; tearing the
    // session down over one would be self-inflicted.
    FixSession session(TestConfig());
    const std::array<Field, 1> body = {
        Field{.tag = tag::kPossDupFlag, .value = "Y"},
    };
    const std::string raw = Inbound(msg_type::kHeartbeat, 1, body);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const InboundCheck check = session.OnInbound(*parsed);
    ASSERT_EQ(check.status, SequenceStatus::kPossibleDuplicate);
    const auto decision = ClassifyInbound(*parsed, check);
    EXPECT_EQ(decision.kind, InboundKind::kHeartbeat);
    EXPECT_EQ(decision.action, InboundAction::kNone);
}

TEST(DeribitInboundDecision, AnUnrecognisedMessageTypeIsJournaledAndOtherwiseIgnored) {
    // "unknown" must stay inert: the message has already been journaled by the
    // time this runs, and a type this build does not know is not a reason to
    // drop a working session.
    const std::string raw = Inbound("n", 2);  // XMLnonFIX, which this build never asked for.
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = ClassifyInbound(*parsed, InSequence(2));
    EXPECT_EQ(decision.kind, InboundKind::kUnknown);
    EXPECT_EQ(decision.action, InboundAction::kNone);
}

TEST(DeribitReconnectBackoff, DoesNotDelayTheFirstConnectionAttempt) {
    EXPECT_EQ(ReconnectDelayMs(0, 1'000, 30'000), 0U);
}

TEST(DeribitReconnectBackoff, DoublesFromTheFloorAndSaturatesAtTheCap) {
    EXPECT_EQ(ReconnectDelayMs(1, 1'000, 30'000), 1'000U);
    EXPECT_EQ(ReconnectDelayMs(2, 1'000, 30'000), 2'000U);
    EXPECT_EQ(ReconnectDelayMs(3, 1'000, 30'000), 4'000U);
    EXPECT_EQ(ReconnectDelayMs(5, 1'000, 30'000), 16'000U);
    EXPECT_EQ(ReconnectDelayMs(6, 1'000, 30'000), 30'000U);
}

TEST(DeribitReconnectBackoff, StaysAtTheCapAcrossALongOutage) {
    // The shift is capped before the clamp so a day-long outage cannot shift a
    // 64-bit value past its width -- that is undefined behaviour, not a large
    // number, and it would be found by an outage rather than by a test run.
    EXPECT_EQ(ReconnectDelayMs(64, 1'000, 30'000), 30'000U);
    EXPECT_EQ(ReconnectDelayMs(100'000, 1'000, 30'000), 30'000U);
}

/// Drives the real FixClient -- real thread, real socket -- against the
/// scripted peer above. Each test gets its own listening port and its own
/// journal directory, and leaves neither behind.
class DeribitFixLoopback : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(server_.Listening()) << "could not open a loopback listening socket";
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
    FixClientConfig LoopbackConfig() const {
        return FixClientConfig{
            .host = "127.0.0.1",
            .port = server_.Port(),
            .symbols = {"BTC-PERPETUAL"},
            .md_req_id = "ct-md-loopback",
            .connect_timeout_ms = kStepTimeoutMs,
            .recv_timeout_ms = kLoopbackRecvTimeoutMs,
            .staleness_timeout_ns = kLoopbackStalenessNs,
            .min_reconnect_wait_ms = 10,
            .max_reconnect_wait_ms = 50,
        };
    }

    LoopbackServer server_;
    std::filesystem::path dir_;
};

TEST_F(DeribitFixLoopback, AnswersATestRequestWithAHeartbeatEchoingItsTestReqId) {
    // An unanswered TestRequest ends the session (exchanges/deribit.md), and an
    // answer carrying the wrong TestReqID is an unanswered one. The decision to
    // reply is unit-tested above; what this adds is that the reply is actually
    // built, sent, and correct on the wire.
    constexpr std::string_view kTestReqId = "TEST-LOOPBACK-4711";

    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClient client(TestConfig(), capture, LoopbackConfig());
    client.Start();

    Peer exchange = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(exchange.Connected()) << "the client never connected";
    ASSERT_TRUE(CompleteHandshake(exchange));

    const std::array<Field, 1> body = {
        Field{.tag = tag::kTestReqId, .value = std::string(kTestReqId)},
    };
    ASSERT_TRUE(exchange.Send(Inbound(msg_type::kTestRequest, 2, body)));

    std::string raw;
    ASSERT_TRUE(ExpectNext(exchange, msg_type::kHeartbeat, raw));
    const auto answer = ParseMessage(raw);
    ASSERT_TRUE(answer.has_value()) << answer.error();
    EXPECT_EQ(answer->Get(tag::kTestReqId).value_or(""), kTestReqId);
    // The reply is a genuine session message, not a bare echo: it carries the
    // next outbound MsgSeqNum (Logon 1, MarketDataRequest 2, Heartbeat 3).
    EXPECT_EQ(answer->Get(tag::kMsgSeqNum).value_or(""), "3");

    client.Stop();
    EXPECT_EQ(client.MessagesReceived(), 2U);  // the Logon and the TestRequest
    EXPECT_EQ(client.ConnectionAttempts(), 1U);
}

TEST_F(DeribitFixLoopback, ASequenceGapDropsTheConnectionAndLogsOnAgain) {
    // decisions/0004: no ResendRequest gap fill -- a gap means drop and
    // re-logon. Proving that end to end needs the socket: the pure function
    // only says "reconnect", it cannot show that a second session happened.
    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClient client(TestConfig(), capture, LoopbackConfig());
    client.Start();

    Peer first = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(first.Connected()) << "the client never connected";
    ASSERT_TRUE(CompleteHandshake(first));
    EXPECT_EQ(client.ConnectionAttempts(), 1U);

    // MsgSeqNum jumps 1 -> 5: three messages the session will never see again.
    ASSERT_TRUE(first.Send(Inbound(msg_type::kMarketDataIncrementalRefresh, 5)));

    // The reconnect itself: a second connection to the same listener, carrying
    // a fresh Logon. connection_attempts_ is incremented before connect(), so
    // by the time this accept() returns the counter has already advanced --
    // there is nothing to poll for and nothing to race with.
    Peer second = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(second.Connected()) << "the client did not reconnect after the gap";
    std::string raw;
    ASSERT_TRUE(ExpectNext(second, msg_type::kLogon, raw));
    EXPECT_GE(client.ConnectionAttempts(), 2U);

    client.Stop();
    // Note for whoever reads this next: forced_reconnects() deliberately stays
    // 0 here. It counts only staleness-forced drops; a gap-driven reconnect is
    // not counted anywhere today.
}

TEST_F(DeribitFixLoopback, StampsEveryCapturedFrameWithItsOwnWireShape) {
    // What an order-book sink fed by both exchanges branches on, proven where
    // the frames are real: the session is opened by the client, and the sink
    // sees what the client actually put into it.
    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    feed_handler::testing::RecordingSink sink;
    capture.AddSink(sink);
    FixClient client(TestConfig(), capture, LoopbackConfig());
    client.Start();

    Peer exchange = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(exchange.Connected()) << "the client never connected";
    // Returns only once the MarketDataRequest is out, which the client sends
    // strictly after journaling the Logon that triggered it.
    ASSERT_TRUE(CompleteHandshake(exchange));

    const auto frames = sink.Frames();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].source, feed_handler::FrameSource::kDeribitFix);
    EXPECT_NE(frames[0].payload.find("35=A"), std::string::npos);
    // Sequence 1 is the connect marker, which is a journal record rather
    // than a frame, so the first frame a sink sees is 2.
    EXPECT_EQ(frames[0].capture_sequence, 2U);

    const auto connects = sink.Connects();
    ASSERT_EQ(connects.size(), 1U);
    EXPECT_EQ(connects[0].connect_id, 1U);
    EXPECT_NE(connects[0].reason.find("deribit fix"), std::string::npos);

    client.Stop();
}

TEST_F(DeribitFixLoopback, ClosesTheJournalWhenAGapDropsTheConnection) {
    // The bug this covers: every exit from the read loop used to leave the
    // journal file open with up to a full 1 MiB write buffer unflushed, until
    // the *next* successful connection's begin_connect() closed it -- which
    // can be a whole reconnect backoff later, or never.
    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClient client(TestConfig(), capture, LoopbackConfig());
    client.Start();

    Peer first = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(first.Connected()) << "the client never connected";
    ASSERT_TRUE(CompleteHandshake(first));

    // Nothing to reconnect to from here on, so connect_id 1's file can only be
    // closed by the path that drops the connection -- not by connect_id 2.
    server_.StopListening();
    // MsgSeqNum jumps 1 -> 5: three messages the session will never see again.
    ASSERT_TRUE(first.Send(Inbound(msg_type::kMarketDataIncrementalRefresh, 5)));

    // The marker, the Logon and the gapped incremental: journaling happens
    // before classification, so the message that ended the session is in the
    // file too.
    EXPECT_TRUE(ExpectClosedJournal(dir_, 1, 3));

    client.Stop();
}

TEST_F(DeribitFixLoopback, ClosesTheJournalWhenTheStalenessWatchdogFires) {
    // The same guarantee on the other reconnect trigger, which leaves the loop
    // through a different return: a half-open connection produces silence, and
    // the file has to be complete on disk before the client goes looking for a
    // new connection.
    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClientConfig cfg = LoopbackConfig();
    // Far shorter than the 5s the other loopback tests use, because here the
    // watchdog firing IS the test rather than something that must not happen.
    cfg.staleness_timeout_ns = 200ULL * 1'000'000ULL;
    FixClient client(TestConfig(), capture, cfg);
    client.Start();

    Peer exchange = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(exchange.Connected()) << "the client never connected";
    ASSERT_TRUE(CompleteHandshake(exchange));
    server_.StopListening();

    // Then simply say nothing: no heartbeats, no data. The marker and the
    // Logon are what the file must contain by the time the watchdog fires.
    EXPECT_TRUE(ExpectClosedJournal(dir_, 1, 2));
    EXPECT_GE(client.ForcedReconnects(), 1U);

    client.Stop();
}

TEST_F(DeribitFixLoopback, SendsItsScheduledHeartbeatWhileInboundDataKeepsFlowing) {
    // The bug this covers: the HeartBtInt check used to sit inside the
    // recv()-timed-out branch, so it only ran on a *quiet* connection. What the
    // exchange is owed is a Heartbeat every HeartBtInt of outbound silence
    // (exchanges/deribit.md), which has nothing to do with inbound traffic -- so
    // on a busy feed, where recv() keeps returning data promptly, the client's
    // own scheduled Heartbeat never fired at all. It escaped notice live only
    // because a TestRequest is answered through a different path.
    SessionConfig session_cfg = TestConfig();
    session_cfg.heartbeat_interval_seconds = kLoopbackHeartbeatSeconds;

    FixClientConfig cfg = LoopbackConfig();
    // Long next to the trickle below: the receive timeout must not be able to
    // expire while this test runs, or the old code would pass it too.
    cfg.recv_timeout_ms = kBusyRecvTimeoutMs;

    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClient client(std::move(session_cfg), capture, cfg);
    client.Start();

    Peer exchange = server_.AcceptOne(kStepTimeoutMs);
    ASSERT_TRUE(exchange.Connected()) << "the client never connected";
    // Returns with the MarketDataRequest taken off the wire, so the next thing
    // the client sends is the thing under test -- and that request is also the
    // last outbound byte, i.e. where the heartbeat interval starts counting.
    ASSERT_TRUE(CompleteHandshake(exchange));

    // Deribit's own heartbeats, in sequence, faster than the client's receive
    // timeout: a continuously busy connection.
    std::uint64_t inbound_seq_num = 2;
    std::optional<std::string> sent;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeartbeatDeadlineMs);
    while (!sent && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(exchange.Send(Inbound(msg_type::kHeartbeat, inbound_seq_num++)))
            << "the client dropped the connection mid-trickle";
        sent = exchange.ReadMessage(kTrickleIntervalMs);
    }

    ASSERT_TRUE(sent.has_value()) << "the client never sent its scheduled Heartbeat in "
                                  << kHeartbeatDeadlineMs << "ms of continuous inbound traffic";
    const auto parsed = ParseMessage(*sent);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->MsgType(), msg_type::kHeartbeat);
    // A scheduled Heartbeat, not an answer to something: no TestRequest was
    // ever sent, so there is no TestReqID(112) to echo.
    EXPECT_FALSE(parsed->Get(tag::kTestReqId).has_value());

    client.Stop();
}

TEST_F(DeribitFixLoopback, RequestStopReturnsWithoutJoiningAndJoinThenCompletes) {
    server_.StopListening();  // The thread sits in the reconnect backoff wait.
    FixClientConfig cfg = LoopbackConfig();
    cfg.min_reconnect_wait_ms = kStuckBackoffMs;
    cfg.max_reconnect_wait_ms = kStuckBackoffMs;

    CaptureSession capture({.directory = dir_, .exchange = "deribit"});
    FixClient client(TestConfig(), capture, cfg);
    client.Start();

    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStepTimeoutMs);
    while (client.ConnectionAttempts() == 0 && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    ASSERT_GE(client.ConnectionAttempts(), 1U) << "the client never tried to connect";

    const auto before = std::chrono::steady_clock::now();
    client.RequestStop();
    const auto request_ms = feed_handler::test_support::ElapsedSince(before).count();
    EXPECT_LT(request_ms, kPromptStopMs) << "RequestStop() should not wait for the thread";

    client.Join();
    const auto joined_ms = feed_handler::test_support::ElapsedSince(before).count();
    EXPECT_LT(joined_ms, kPromptStopMs)
        << "Join() waited out the " << kStuckBackoffMs << "ms backoff instead of being woken";

    // Every later call is harmless, and so is the destructor after them.
    client.RequestStop();
    client.Join();
    client.Stop();
}

TEST_F(DeribitFixLoopback, StopsPromptlyWhileWaitingOutTheReconnectBackoff) {
    // A notify issued without the StopSignal's mutex held can land in the window
    // between a waiter evaluating the predicate and its wait actually
    // registering, and the wakeup is then delivered to nobody. Not a deadlock --
    // the timeout still expires -- but shutdown then sleeps out the whole
    // backoff, up to max_reconnect_wait_ms (30s in production). A true race
    // cannot be hit on demand, so what is asserted is the property the fix
    // guarantees: stop() is bounded well below the wait it interrupts, every
    // time, with each attempt racing stop() against the thread entering that
    // wait.
    server_.StopListening();  // Every connect() is refused immediately, so the
                              // thread is in the backoff wait and nowhere else.
    FixClientConfig cfg = LoopbackConfig();
    cfg.min_reconnect_wait_ms = kStuckBackoffMs;
    cfg.max_reconnect_wait_ms = kStuckBackoffMs;

    for (int attempt = 0; attempt < kStopRaceAttempts; ++attempt) {
        CaptureSession capture({.directory = dir_, .exchange = "deribit"});
        FixClient client(TestConfig(), capture, cfg);
        client.Start();

        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kStepTimeoutMs);
        while (client.ConnectionAttempts() == 0 && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::yield();
        }
        ASSERT_GE(client.ConnectionAttempts(), 1U) << "the client never tried to connect";

        const auto before = std::chrono::steady_clock::now();
        client.Stop();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count();
        ASSERT_LT(elapsed_ms, kPromptStopMs)
            << "stop() took " << elapsed_ms << "ms on attempt " << attempt << ": it waited out the "
            << kStuckBackoffMs << "ms backoff instead of being woken";
    }
}
