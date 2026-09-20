// CaptureSet with no live network. Kraken entries are only ever built here, never
// started: StartAll runs the Kraken instrument-reference lookup, which is a real
// REST call to Kraken.
#include "feed_handler/capture_set.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/runner.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureSet;
using feed_handler::Credential;
using feed_handler::config::Connection;
using feed_handler::config::FeedHandlerConfig;
using feed_handler::test_support::DeribitEntry;
using feed_handler::test_support::kFakeCredential;
using feed_handler::test_support::KrakenEntry;
using feed_handler::test_support::MustParse;
using feed_handler::test_support::UniqueTestDir;

class CaptureSetTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = UniqueTestDir("capture_set");
        config_ =
            MustParse("journal_dir = \"" + (dir_ / "journal").string() + "\"\nstate_dir = \"" +
                      (dir_ / "state").string() + "\"\n" +
                      KrakenEntry("kraken-a", "BTC/USD", "endpoint = \"ws://127.0.0.1:1\"\n") +
                      DeribitEntry("deribit-a", "BTC-PERPETUAL", "endpoint = \"127.0.0.1:1\"\n") +
                      DeribitEntry("deribit-b", "ETH-PERPETUAL", "endpoint = \"127.0.0.1:1\"\n"));
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    /// The entries at `indices` of the parsed config, in the order given.
    std::vector<Connection> Pick(std::initializer_list<std::size_t> indices) const {
        std::vector<Connection> picked;
        for (const std::size_t index : indices) {
            picked.push_back(config_.connections[index]);
        }
        return picked;
    }

    static std::vector<Credential> Credentials(std::size_t count) {
        return std::vector<Credential>(count, kFakeCredential);
    }

    FeedHandlerConfig config_;
    std::filesystem::path dir_;
};

TEST_F(CaptureSetTest, BuildKeepsTheEntriesInTheOrderGiven) {
    const auto connections = Pick({1, 0, 2});
    const auto credentials = Credentials(connections.size());
    const auto set = CaptureSet::Build(config_, connections, credentials);
    ASSERT_TRUE(set.has_value()) << set.error();

    ASSERT_EQ(set->Connections().size(), 3U);
    EXPECT_EQ(set->Connections()[0]->Id(), "deribit-a");
    EXPECT_EQ(set->Connections()[1]->Id(), "kraken-a");
    EXPECT_EQ(set->Connections()[2]->Id(), "deribit-b");
}

TEST_F(CaptureSetTest, ACountMismatchBetweenConnectionsAndCredentialsIsAnError) {
    const auto connections = Pick({0, 1});
    const auto credentials = Credentials(1);
    const auto set = CaptureSet::Build(config_, connections, credentials);
    ASSERT_FALSE(set.has_value());
    EXPECT_NE(set.error().find("2 connections but 1 credentials"), std::string::npos)
        << set.error();
}

TEST_F(CaptureSetTest, ARestClientExistsOnlyWhenAKrakenConnectionIsSelected) {
    const auto deribit_only = Pick({1, 2});
    const auto deribit_credentials = Credentials(deribit_only.size());
    const auto without = CaptureSet::Build(config_, deribit_only, deribit_credentials);
    ASSERT_TRUE(without.has_value()) << without.error();
    EXPECT_FALSE(without->HasRestClient());

    const auto with_kraken = Pick({0, 1});
    const auto kraken_credentials = Credentials(with_kraken.size());
    const auto with = CaptureSet::Build(config_, with_kraken, kraken_credentials);
    ASSERT_TRUE(with.has_value()) << with.error();
    EXPECT_TRUE(with->HasRestClient());
}

TEST_F(CaptureSetTest, BuildStartsNothingAndTouchesNoFilesystem) {
    const auto connections = Pick({0, 1, 2});
    const auto credentials = Credentials(connections.size());
    const auto set = CaptureSet::Build(config_, connections, credentials);
    ASSERT_TRUE(set.has_value()) << set.error();
    EXPECT_FALSE(std::filesystem::exists(dir_));
}

TEST_F(CaptureSetTest, DestroyingASetThatWasNeverStartedIsClean) {
    // Kraken and Deribit both present: the connections that reference the
    // RestClient are destroyed before it.
    const auto connections = Pick({0, 1, 2});
    const auto credentials = Credentials(connections.size());
    {
        auto set = CaptureSet::Build(config_, connections, credentials);
        ASSERT_TRUE(set.has_value()) << set.error();
    }
    SUCCEED();
}

TEST_F(CaptureSetTest, ASetIsMovableAndTheMovedToSetOwnsTheConnections) {
    const auto connections = Pick({0, 1});
    const auto credentials = Credentials(connections.size());
    auto built = CaptureSet::Build(config_, connections, credentials);
    ASSERT_TRUE(built.has_value()) << built.error();

    CaptureSet moved = std::move(*built);
    EXPECT_EQ(moved.Connections().size(), 2U);
    EXPECT_TRUE(moved.HasRestClient());
}

TEST_F(CaptureSetTest, ADeribitOnlySetStartsRunsAndStopsWithoutHanging) {
    // Unreachable endpoints: each connection just retries in the background.
    const auto connections = Pick({1, 2});
    const auto credentials = Credentials(connections.size());
    auto set = CaptureSet::Build(config_, connections, credentials);
    ASSERT_TRUE(set.has_value()) << set.error();

    set->StartAll();
    int polls = 0;
    const auto result = feed_handler::Run(
        set->Connections(),
        {.poll = std::chrono::milliseconds{10}, .should_stop = [&polls] { return ++polls > 3; }});
    EXPECT_FALSE(result.Fatal());
}

}  // namespace
