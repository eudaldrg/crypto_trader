// CaptureSet with no live network. Kraken entries are only ever built here, never
// started: StartAll runs the Kraken instrument-reference lookup, which is a real
// REST call to Kraken.
#include "feed_handler/capture_set.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "feed_handler/capture_connection.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/runner.h"
#include "feed_handler/tests/recording_sink.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureSet;
using feed_handler::ResolvedConnection;
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

    /// The entries at `indices` of the parsed config, in the order given, each
    /// with a fake credential.
    std::vector<ResolvedConnection> Pick(std::initializer_list<std::size_t> indices) const {
        std::vector<ResolvedConnection> picked;
        for (const std::size_t index : indices) {
            picked.push_back(
                {.connection = config_.connections[index], .credential = kFakeCredential});
        }
        return picked;
    }

    FeedHandlerConfig config_;
    std::filesystem::path dir_;
};

TEST_F(CaptureSetTest, BuildKeepsTheEntriesInTheOrderGiven) {
    const auto set = CaptureSet::Build(config_, Pick({1, 0, 2}));
    ASSERT_TRUE(set.has_value()) << set.error();

    ASSERT_EQ(set->Connections().size(), 3U);
    EXPECT_EQ(set->Connections()[0]->Id(), "deribit-a");
    EXPECT_EQ(set->Connections()[1]->Id(), "kraken-a");
    EXPECT_EQ(set->Connections()[2]->Id(), "deribit-b");
}

TEST_F(CaptureSetTest, ARestClientExistsOnlyWhenAKrakenConnectionIsSelected) {
    const auto without = CaptureSet::Build(config_, Pick({1, 2}));
    ASSERT_TRUE(without.has_value()) << without.error();
    EXPECT_FALSE(without->HasRestClient());

    const auto with = CaptureSet::Build(config_, Pick({0, 1}));
    ASSERT_TRUE(with.has_value()) << with.error();
    EXPECT_TRUE(with->HasRestClient());
}

TEST_F(CaptureSetTest, BuildStartsNothingAndTouchesNoFilesystem) {
    const auto set = CaptureSet::Build(config_, Pick({0, 1, 2}));
    ASSERT_TRUE(set.has_value()) << set.error();
    EXPECT_FALSE(std::filesystem::exists(dir_));
}

TEST_F(CaptureSetTest, DestroyingASetThatWasNeverStartedIsClean) {
    // Kraken and Deribit both present: the connections that reference the
    // RestClient are destroyed before it.
    {
        auto set = CaptureSet::Build(config_, Pick({0, 1, 2}));
        ASSERT_TRUE(set.has_value()) << set.error();
    }
    SUCCEED();
}

TEST_F(CaptureSetTest, ASetIsMovableAndTheMovedToSetOwnsTheConnections) {
    auto built = CaptureSet::Build(config_, Pick({0, 1}));
    ASSERT_TRUE(built.has_value()) << built.error();

    CaptureSet moved = std::move(*built);
    EXPECT_EQ(moved.Connections().size(), 2U);
    EXPECT_TRUE(moved.HasRestClient());
}

TEST_F(CaptureSetTest, ADeribitOnlySetStartsRunsAndStopsWithoutHanging) {
    // Unreachable endpoints: each connection just retries in the background.
    auto set = CaptureSet::Build(config_, Pick({1, 2}));
    ASSERT_TRUE(set.has_value()) << set.error();

    set->StartAll();
    int polls = 0;
    const auto result = feed_handler::Run(
        set->Connections(),
        {.poll = std::chrono::milliseconds{10}, .should_stop = [&polls] { return ++polls > 3; }});
    EXPECT_FALSE(result.Fatal());
}

TEST_F(CaptureSetTest,
       StartAllOffersEveryConnectionToTheHookInOrderAndWithoutARestClientForDeribit) {
    auto set = CaptureSet::Build(config_, Pick({1, 2}));
    ASSERT_TRUE(set.has_value()) << set.error();

    std::vector<std::string> offered;
    std::size_t with_rest = 0;
    set->StartAll([&](feed_handler::CaptureConnection& connection,
                      const feed_handler::config::Connection& entry,
                      const feed_handler::kraken::RestClient* rest) {
        offered.emplace_back(connection.Id());
        EXPECT_EQ(entry.id, connection.Id());
        with_rest += rest != nullptr ? 1U : 0U;
        // The point of the hook: a sink can still be added, the connection has not
        // started.
        feed_handler::testing::RecordingSink sink;
        connection.AddSink(sink);
    });
    int polls = 0;
    feed_handler::Run(set->Connections(), {.poll = std::chrono::milliseconds{10},
                                           .should_stop = [&polls] { return ++polls > 3; }});

    EXPECT_EQ(offered, (std::vector<std::string>{"deribit-a", "deribit-b"}));
    EXPECT_EQ(with_rest, 0U);
}

}  // namespace
