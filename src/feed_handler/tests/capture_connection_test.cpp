// The per-exchange CaptureConnection objects, with no live network: Kraken
// points at a refused loopback URL and Deribit at a refused loopback port, so a
// started connection just retries in the background until it is stopped.
//
// The last group runs them with the threaded journal they are built with
// (ClientCapture): frames in, a complete file after Join, and a journal failure
// ending in Fatal() and in the runner's fatal result, which is the process's
// exit code 1. Kraken's frames are fed to the client directly (IXWebSocket owns
// the socket, there is no loopback harness for it) and Deribit's arrive over a
// bare loopback listener.
#include "feed_handler/capture_connection.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "feed_handler/capture_session.h"
#include "feed_handler/client_capture.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/deribit/deribit_capture.h"
#include "feed_handler/fix/fix_message.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/kraken/kraken_capture.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/kraken/kraken_ws_client.h"
#include "feed_handler/runner.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureConnection;
using feed_handler::CaptureSession;
using feed_handler::FrameSource;
using feed_handler::config::FeedHandlerConfig;
using feed_handler::test_support::DeribitEntry;
using feed_handler::test_support::ElapsedSince;
using feed_handler::test_support::kFakeCredential;
using feed_handler::test_support::KrakenEntry;
using feed_handler::test_support::MustParse;
using feed_handler::test_support::UniqueTestDir;

constexpr std::chrono::milliseconds kBound{3'000};
/// How long a step that waits on another thread may take before the test calls
/// it failed. Far above what any of them needs; it only has to end a wedged test.
constexpr std::chrono::milliseconds kStepTimeout{5'000};
constexpr std::size_t kUpdates = 20;
constexpr std::string_view kUpdate =
    R"({"channel":"level3","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[],)"
    R"("checksum":1671312190}]})";

class CaptureConnections : public ::testing::Test {
  protected:
    void SetUp() override {
        journal_dir_ = UniqueTestDir("capture_connections");
        config_ =
            MustParse("journal_dir = \"" + journal_dir_.string() + "\"\n" +
                      KrakenEntry("kraken-a", "BTC/USD", "endpoint = \"ws://127.0.0.1:1\"\n") +
                      DeribitEntry("deribit-a", "BTC-PERPETUAL", "endpoint = \"127.0.0.1:1\"\n"));
    }

    void TearDown() override {
        std::filesystem::remove_all(journal_dir_);
    }

    std::unique_ptr<CaptureConnection> MakeKraken() {
        return feed_handler::kraken::MakeKrakenCapture(config_, config_.connections[0],
                                                       kFakeCredential, rest_);
    }

    std::unique_ptr<CaptureConnection> MakeDeribit() {
        return feed_handler::deribit::MakeDeribitCapture(config_, config_.connections[1],
                                                         kFakeCredential);
    }

    /// Start, request stop, join: how long the whole shutdown took.
    static std::chrono::milliseconds StartAndStop(CaptureConnection& connection) {
        const auto before = std::chrono::steady_clock::now();
        connection.Start();
        connection.RequestStop();
        connection.Join();
        return ElapsedSince(before);
    }

    FeedHandlerConfig config_;
    std::filesystem::path journal_dir_;
    feed_handler::kraken::RestClient rest_;
};

TEST_F(CaptureConnections, TheIdIsTheConfigsConnectionId) {
    EXPECT_EQ(MakeKraken()->Id(), "kraken-a");
    EXPECT_EQ(MakeDeribit()->Id(), "deribit-a");
}

TEST_F(CaptureConnections, ASummaryIsTaggedWithTheIdAndCountsNothingBeforeStart) {
    const std::string kraken = MakeKraken()->Summary();
    EXPECT_NE(kraken.find("[kraken-a] captured 0 messages"), std::string::npos) << kraken;

    const std::string deribit = MakeDeribit()->Summary();
    EXPECT_NE(deribit.find("[deribit-a] captured 0 messages (0 snapshot(s), 0 incremental(s))"),
              std::string::npos)
        << deribit;
    EXPECT_NE(deribit.find("0 connection attempt(s)"), std::string::npos) << deribit;
}

TEST_F(CaptureConnections, NothingIsJournaledOrFatalBeforeStart) {
    const auto kraken = MakeKraken();
    const auto deribit = MakeDeribit();
    EXPECT_FALSE(kraken->Fatal());
    EXPECT_FALSE(deribit->Fatal());
    // No session has begun a connect, so no directory or file exists yet.
    EXPECT_FALSE(std::filesystem::exists(journal_dir_));
}

TEST_F(CaptureConnections, AStartedKrakenConnectionStopsPromptlyAndIsNotFatal) {
    const auto connection = MakeKraken();
    EXPECT_LT(StartAndStop(*connection), kBound);
    EXPECT_FALSE(connection->Fatal());
}

TEST_F(CaptureConnections, AStartedDeribitConnectionStopsPromptlyAndIsNotFatal) {
    const auto connection = MakeDeribit();
    EXPECT_LT(StartAndStop(*connection), kBound);
    EXPECT_FALSE(connection->Fatal());
}

TEST_F(CaptureConnections, JoinAndDestructionAfterAStopAreHarmless) {
    auto kraken = MakeKraken();
    kraken->Start();
    kraken->RequestStop();
    kraken->Join();
    kraken->Join();
    kraken->RequestStop();
    kraken.reset();

    auto deribit = MakeDeribit();
    deribit->Start();
    deribit->Join();  // Requests the stop itself when nobody did.
    deribit->Join();
    deribit.reset();
}

TEST_F(CaptureConnections, AConnectionAbandonedWhileRunningStopsInItsDestructor) {
    // An early return in a caller must not leave a thread running.
    {
        auto kraken = MakeKraken();
        kraken->Start();
    }
    {
        auto deribit = MakeDeribit();
        deribit->Start();
    }
    SUCCEED();
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// The one journal file `directory` holds for `id`'s connect `connect_id`, empty
/// when there is none.
std::filesystem::path JournalFor(const std::filesystem::path& directory, std::string_view id,
                                 std::uint64_t connect_id) {
    char ordinal[16] = {};
    std::snprintf(ordinal, sizeof(ordinal), "%06llu", static_cast<unsigned long long>(connect_id));
    const std::string prefix = std::string(id) + "-" + ordinal + "-";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().filename().string().starts_with(prefix)) {
            return entry.path();
        }
    }
    return {};
}

/// How many records `path` holds, or nullopt when it does not read to a clean end.
std::optional<std::size_t> CountRecords(const std::filesystem::path& path) {
    auto reader = feed_handler::JournalReader::Open(path);
    if (!reader) {
        return std::nullopt;
    }
    std::size_t records = 0;
    while (reader->Next().has_value()) {
        ++records;
    }
    if (reader->StoppedEarly()) {
        return std::nullopt;
    }
    return records;
}

/// A real Kraken connection object (the threaded session, the fatal handler, the
/// teardown contract) whose frames come from the test instead of a socket:
/// IXWebSocket owns the socket, so it cannot be scripted. The client is never
/// started; the session is opened the way HandleOpen() opens it.
class DrivenKrakenCapture final
    : public feed_handler::ClientCapture<feed_handler::kraken::WsClient> {
  public:
    DrivenKrakenCapture(const FeedHandlerConfig& config, feed_handler::kraken::RestClient& rest)
        : ClientCapture(config, config.connections[0], [&](CaptureSession& session) {
              return feed_handler::kraken::WsClient(
                  rest,
                  feed_handler::kraken::Credentials{.api_key = "not-a-key",
                                                    .api_secret_b64 = "bm90LWEtcmVhbC1zZWNyZXQ="},
                  session,
                  feed_handler::kraken::WsClientConfig{.url = config.connections[0].endpoint,
                                                       .id = config.connections[0].id,
                                                       .symbols = config.connections[0].symbols});
          }) {}

    using ClientCapture::GetClient;
    using ClientCapture::JournalSession;

    std::string Summary() const override {
        return "driven";
    }
};

/// A listening socket on 127.0.0.1 (port 0, so the kernel picks a free one) that
/// sends what it is told to whoever connects and reads nothing.
class LoopbackListener {
  public:
    LoopbackListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        ::sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(0);
        ::socklen_t length = sizeof(address);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(fd_, reinterpret_cast<::sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, 4) != 0 ||
            ::getsockname(fd_, reinterpret_cast<::sockaddr*>(&address), &length) != 0) {
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
            ::close(fd_);
            fd_ = -1;
            return;
        }
        port_ = ::ntohs(address.sin_port);
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    ~LoopbackListener() {
        if (peer_ >= 0) {
            ::close(peer_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool Listening() const {
        return fd_ >= 0;
    }

    std::uint16_t Port() const {
        return port_;
    }

    /// Waits for one client, then sends it `bytes`. False if none came in time.
    bool AcceptAndSend(std::string_view bytes) {
        ::pollfd waiting{.fd = fd_, .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, static_cast<int>(kStepTimeout.count())) != 1) {
            return false;
        }
        peer_ = ::accept(fd_, nullptr, nullptr);
        return peer_ >= 0 && ::send(peer_, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
                                 static_cast<ssize_t>(bytes.size());
    }

  private:
    int fd_ = -1;
    int peer_ = -1;
    std::uint16_t port_ = 0;
};

/// What Deribit would send after a Logon: the accepted Logon, then Heartbeats in
/// sequence, addressed to `client_id`. Every one of them is journaled.
std::string InboundSession(std::string_view client_id, std::size_t heartbeats) {
    const auto message = [&](std::string_view type, std::uint64_t sequence,
                             std::span<const feed_handler::fix::Field> body) {
        return feed_handler::fix::BuildMessage(
            feed_handler::fix::SessionHeader{.msg_type = type,
                                             .sender_comp_id = "DERIBITSERVER",
                                             .target_comp_id = client_id,
                                             .msg_seq_num = sequence,
                                             .sending_time = "20260916-21:30:00.000"},
            body);
    };
    const std::array<feed_handler::fix::Field, 2> logon_body = {
        feed_handler::fix::Field{.tag = feed_handler::fix::tag::kEncryptMethod, .value = "0"},
        feed_handler::fix::Field{.tag = feed_handler::fix::tag::kHeartBtInt, .value = "30"},
    };
    std::string bytes = message(feed_handler::fix::msg_type::kLogon, 1, logon_body);
    for (std::size_t index = 0; index < heartbeats; ++index) {
        bytes += message(feed_handler::fix::msg_type::kHeartbeat, index + 2, {});
    }
    return bytes;
}

/// Polls `done` until it is true or kStepTimeout passes.
template <class Predicate>
bool WaitUntil(Predicate&& done) {
    const auto deadline = std::chrono::steady_clock::now() + kStepTimeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

/// The runner, as main() drives it, with a stop that only a wedged test reaches:
/// returns the RunResult, whose Fatal() is what makes the process exit 1.
feed_handler::RunResult RunUntilFatalOrTimeout(std::unique_ptr<CaptureConnection>& connection) {
    const auto deadline = std::chrono::steady_clock::now() + kStepTimeout;
    const std::span<const std::unique_ptr<CaptureConnection>> connections(&connection, 1);
    return feed_handler::Run(connections,
                             {.poll = std::chrono::milliseconds(5), .should_stop = [deadline] {
                                  return std::chrono::steady_clock::now() >= deadline;
                              }});
}

TEST_F(CaptureConnections,
       AKrakenConnectionJournalsThroughTheThreadedJournalAndJoinCompletesTheFile) {
    DrivenKrakenCapture connection(config_, rest_);
    ASSERT_TRUE(
        connection.JournalSession().BeginConnect("driven", FrameSource::kKrakenJson).has_value());
    for (std::size_t index = 0; index < kUpdates; ++index) {
        connection.GetClient().HandleMessage(std::string(kUpdate));
    }
    EXPECT_EQ(connection.GetClient().MessagesReceived(), kUpdates);

    // The barrier: the file is complete and readable as soon as Join() returns.
    connection.Join();
    EXPECT_FALSE(connection.Fatal());
    const auto path = JournalFor(journal_dir_, "kraken-a", 1);
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(CountRecords(path), kUpdates + 1U);  // the frames and the connect marker
}

TEST_F(CaptureConnections, AKrakenJournalFailureOnTheJournalThreadEndsInTheRunnersFatalResult) {
    std::unique_ptr<CaptureConnection> connection;
    DrivenKrakenCapture* driven = nullptr;
    {
        auto owned = std::make_unique<DrivenKrakenCapture>(config_, rest_);
        driven = owned.get();
        connection = std::move(owned);
    }
    ASSERT_TRUE(
        driven->JournalSession().BeginConnect("driven", FrameSource::kKrakenJson).has_value());
    ASSERT_FALSE(connection->Fatal());
    {
        // Over the format's limit, so the writer on the journal thread refuses it
        // the way a full disk would. The one large payload in these tests.
        const std::string oversized(feed_handler::journal::kMaxPayloadBytes + 1U, 'x');
        ASSERT_TRUE(
            driven->JournalSession().OnWireMessage(BytesOf(oversized), FrameSource::kKrakenJson));
    }

    // The failure arrives from a thread the client does not own; the runner sees
    // it on a poll, reports the connection and stops it. That result is the
    // process exiting with code 1.
    const feed_handler::RunResult result = RunUntilFatalOrTimeout(connection);
    EXPECT_TRUE(result.Fatal());
    EXPECT_EQ(result.fatal_id, "kraken-a");
    EXPECT_TRUE(connection->Fatal());
}

TEST_F(CaptureConnections,
       ADeribitConnectionJournalsThroughTheThreadedJournalAndJoinCompletesTheFile) {
    LoopbackListener exchange;
    ASSERT_TRUE(exchange.Listening());
    const FeedHandlerConfig config = MustParse(
        "journal_dir = \"" + journal_dir_.string() + "\"\n" +
        DeribitEntry("deribit-a", "BTC-PERPETUAL",
                     "endpoint = \"127.0.0.1:" + std::to_string(exchange.Port()) + "\"\n"));
    const auto connection =
        feed_handler::deribit::MakeDeribitCapture(config, config.connections[0], kFakeCredential);

    connection->Start();
    // The accepted Logon and three Heartbeats, all in sequence.
    ASSERT_TRUE(exchange.AcceptAndSend(InboundSession(kFakeCredential.key, 3)));
    ASSERT_TRUE(WaitUntil([&] {
        return connection->Summary().find("captured 4 messages") != std::string::npos;
    })) << connection->Summary();

    connection->RequestStop();
    connection->Join();
    EXPECT_FALSE(connection->Fatal());
    const auto path = JournalFor(journal_dir_, "deribit-a", 1);
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(CountRecords(path), 5U);  // the four messages and the connect marker
}

TEST_F(CaptureConnections, ADeribitJournalThatCannotBeOpenedEndsInTheRunnersFatalResult) {
    // Opening the file at connect stays synchronous even with the journal on its
    // own thread, so an unusable journal directory is fatal at once. The
    // directory here is under a regular file, which nothing can create.
    const std::filesystem::path blocker = journal_dir_.string() + "_blocker";
    { std::ofstream(blocker) << "not a directory"; }
    LoopbackListener exchange;
    ASSERT_TRUE(exchange.Listening());
    const FeedHandlerConfig config = MustParse(
        "journal_dir = \"" + (blocker / "journal").string() + "\"\n" +
        DeribitEntry("deribit-a", "BTC-PERPETUAL",
                     "endpoint = \"127.0.0.1:" + std::to_string(exchange.Port()) + "\"\n"));
    auto connection =
        feed_handler::deribit::MakeDeribitCapture(config, config.connections[0], kFakeCredential);

    connection->Start();
    const feed_handler::RunResult result = RunUntilFatalOrTimeout(connection);
    std::filesystem::remove(blocker);

    EXPECT_TRUE(result.Fatal());
    EXPECT_EQ(result.fatal_id, "deribit-a");
}

}  // namespace
