// The per-exchange CaptureConnection objects, with no live network: Kraken
// points at a refused loopback URL and Deribit at a refused loopback port, so a
// started connection just retries in the background until it is stopped.
#include "feed_handler/capture_connection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/deribit/deribit_capture.h"
#include "feed_handler/kraken/kraken_capture.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureConnection;
using feed_handler::config::FeedHandlerConfig;
using feed_handler::test_support::DeribitEntry;
using feed_handler::test_support::ElapsedSince;
using feed_handler::test_support::kFakeCredential;
using feed_handler::test_support::KrakenEntry;
using feed_handler::test_support::MustParse;
using feed_handler::test_support::UniqueTestDir;

constexpr std::chrono::milliseconds kBound{3'000};

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

}  // namespace
