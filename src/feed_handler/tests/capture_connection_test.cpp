// The per-exchange CaptureConnection objects, with no live network: Kraken
// points at a refused loopback URL and Deribit at a refused loopback port, so a
// started connection just retries in the background until it is stopped.
#include "feed_handler/capture_connection.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/deribit/deribit_capture.h"
#include "feed_handler/kraken/kraken_capture.h"
#include "feed_handler/kraken/kraken_rest_client.h"

namespace {

using feed_handler::CaptureConnection;
using feed_handler::Credential;
using feed_handler::config::FeedHandlerConfig;

constexpr std::chrono::milliseconds kBound{3'000};

/// Fake values, not credentials; the Kraken secret is base64 of "fake-secret".
const Credential kFakeCredential{.key = "fake-key-not-a-credential", .secret = "ZmFrZS1zZWNyZXQ="};

class CaptureConnections : public ::testing::Test {
  protected:
    void SetUp() override {
        journal_dir_ =
            std::filesystem::temp_directory_path() /
            ("capture_connections_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
             std::to_string(::getpid()));
        std::filesystem::remove_all(journal_dir_);

        const auto parsed =
            feed_handler::config::ParseConfig("journal_dir = \"" + journal_dir_.string() + R"("

[[connections]]
id = "kraken-a"
exchange = "kraken"
env = "prod"
symbols = ["BTC/USD"]
endpoint = "ws://127.0.0.1:1"
api_key_env = "K_KEY"
api_secret_env = "K_SECRET"

[[connections]]
id = "deribit-a"
exchange = "deribit"
env = "testnet"
symbols = ["BTC-PERPETUAL"]
endpoint = "127.0.0.1:1"
api_key_env = "D_KEY"
api_secret_env = "D_SECRET"
)");
        ASSERT_TRUE(parsed.has_value()) << parsed.error();
        config_ = *parsed;
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
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before);
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
    // No session has begun an incarnation, so no directory or file exists yet.
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

}  // namespace
