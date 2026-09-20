#include "feed_handler/selection.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

using feed_handler::CommandLine;
using feed_handler::ParseCommandLine;
using feed_handler::SelectConnections;
using feed_handler::config::Exchange;
using feed_handler::config::FeedHandlerConfig;
using feed_handler::config::ParseConfig;

std::expected<CommandLine, std::string> Parse(std::initializer_list<const char*> args) {
    const std::vector<const char*> argv(args);
    return ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

/// Two Kraken entries and one Deribit entry, in that file order.
FeedHandlerConfig ThreeConnections() {
    const auto config = ParseConfig(R"(
[[connections]]
id = "kraken-a"
exchange = "kraken"
env = "prod"
symbols = ["BTC/USD"]
api_key_env = "K_KEY"
api_secret_env = "K_SECRET"

[[connections]]
id = "deribit-a"
exchange = "deribit"
env = "testnet"
symbols = ["BTC-PERPETUAL"]
api_key_env = "D_KEY"
api_secret_env = "D_SECRET"

[[connections]]
id = "kraken-b"
exchange = "kraken"
env = "prod"
symbols = ["ETH/USD"]
api_key_env = "K_KEY"
api_secret_env = "K_SECRET"
)");
    EXPECT_TRUE(config.has_value()) << (config ? "" : config.error());
    return *config;
}

std::vector<std::string> Ids(const std::vector<feed_handler::config::Connection>& connections) {
    std::vector<std::string> ids;
    for (const auto& connection : connections) {
        ids.push_back(connection.id);
    }
    return ids;
}

TEST(FeedHandlerSelection, ParsesBothFlagSpellings) {
    const auto spaced =
        Parse({"prog", "--config", "a.toml", "--exchange", "kraken", "--only", "x"});
    ASSERT_TRUE(spaced.has_value()) << spaced.error();
    EXPECT_EQ(spaced->config_path, "a.toml");
    EXPECT_EQ(spaced->exchange, Exchange::kKraken);
    EXPECT_EQ(spaced->only_ids, (std::vector<std::string>{"x"}));

    const auto joined = Parse({"prog", "--config=b.toml", "--exchange=deribit", "--only=y"});
    ASSERT_TRUE(joined.has_value()) << joined.error();
    EXPECT_EQ(joined->config_path, "b.toml");
    EXPECT_EQ(joined->exchange, Exchange::kDeribit);
    EXPECT_EQ(joined->only_ids, (std::vector<std::string>{"y"}));
}

TEST(FeedHandlerSelection, OnlyAccumulatesAcrossRepeatsAndCommaLists) {
    const auto parsed = Parse({"prog", "--config", "a", "--only", "x,y", "--only=z"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->only_ids, (std::vector<std::string>{"x", "y", "z"}));
}

TEST(FeedHandlerSelection, OnlyFilterAndExchangeAreOptional) {
    const auto parsed = Parse({"prog", "--config", "a"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_TRUE(parsed->only_ids.empty());
    EXPECT_FALSE(parsed->exchange.has_value());
}

TEST(FeedHandlerSelection, RejectsUnknownAndMalformedArguments) {
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"prog", "--config", "a", "--verbose"},
             {"prog", "--config", "a", "stray"},
             {"prog", "--config"},
             {"prog", "--config="},
             {"prog", "--config", "a", "--only"},
             {"prog", "--config", "a", "--only", "x,,y"},
             {"prog", "--config", "a", "--only", "x,"},
             {"prog", "--config", "a", "--exchange", "binance"},
         }) {
        const auto parsed = ParseCommandLine(static_cast<int>(args.size()), args.data());
        EXPECT_FALSE(parsed.has_value()) << "accepted: " << args.back();
    }
}

TEST(FeedHandlerSelection, RejectsRepeatedConfigAndExchange) {
    EXPECT_FALSE(Parse({"prog", "--config", "a", "--config", "b"}).has_value());
    EXPECT_FALSE(
        Parse({"prog", "--config", "a", "--exchange", "kraken", "--exchange=deribit"}).has_value());
}

TEST(FeedHandlerSelection, ConfigIsRequired) {
    const auto parsed = Parse({"prog", "--exchange", "kraken"});
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("--config is required"), std::string::npos) << parsed.error();
}

TEST(FeedHandlerSelection, EveryErrorCarriesTheUsageLineNamingTheProgram) {
    const auto parsed = Parse({"./feed_handler", "--bogus"});
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("usage: ./feed_handler --config <path>"), std::string::npos)
        << parsed.error();
    EXPECT_NE(parsed.error().find("unknown argument '--bogus'"), std::string::npos);
}

TEST(FeedHandlerSelection, WithNoFiltersEveryConnectionIsSelectedInFileOrder) {
    const auto selected = SelectConnections(ThreeConnections(), CommandLine{});
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(Ids(*selected), (std::vector<std::string>{"kraken-a", "deribit-a", "kraken-b"}));
}

TEST(FeedHandlerSelection, ExchangeSelectsThatExchangesEntriesInFileOrder) {
    const auto selected =
        SelectConnections(ThreeConnections(), CommandLine{.exchange = Exchange::kKraken});
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(Ids(*selected), (std::vector<std::string>{"kraken-a", "kraken-b"}));
}

TEST(FeedHandlerSelection, OnlyKeepsFileOrderNotTheOrderGiven) {
    const auto selected =
        SelectConnections(ThreeConnections(), CommandLine{.only_ids = {"kraken-b", "kraken-a"}});
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(Ids(*selected), (std::vector<std::string>{"kraken-a", "kraken-b"}));
}

TEST(FeedHandlerSelection, ExchangeAndOnlyNarrowTogether) {
    const auto selected = SelectConnections(
        ThreeConnections(),
        CommandLine{.only_ids = {"kraken-a", "deribit-a"}, .exchange = Exchange::kKraken});
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(Ids(*selected), (std::vector<std::string>{"kraken-a"}));
}

TEST(FeedHandlerSelection, AnOnlyIdOfTheOtherExchangeIsAnEmptySelectionError) {
    const auto selected = SelectConnections(
        ThreeConnections(), CommandLine{.only_ids = {"deribit-a"}, .exchange = Exchange::kKraken});
    ASSERT_FALSE(selected.has_value());
    EXPECT_NE(selected.error().find("no configured connection matches"), std::string::npos)
        << selected.error();
}

TEST(FeedHandlerSelection, AnUnknownOnlyIdIsAnErrorNamingItAndTheConfiguredOnes) {
    const auto selected =
        SelectConnections(ThreeConnections(), CommandLine{.only_ids = {"kraken-a", "nope"}});
    ASSERT_FALSE(selected.has_value());
    EXPECT_NE(selected.error().find("'nope'"), std::string::npos) << selected.error();
    EXPECT_NE(selected.error().find("deribit-a"), std::string::npos) << selected.error();
}

TEST(FeedHandlerSelection, AnEmptyConfigurationIsAnErrorNotAnEmptyCapture) {
    const auto selected = SelectConnections(FeedHandlerConfig{}, CommandLine{});
    ASSERT_FALSE(selected.has_value());
}

}  // namespace
