#include "feed_handler/config/feed_handler_config.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

namespace {

using feed_handler::config::ConfigPathFromArgs;
using feed_handler::config::Environment;
using feed_handler::config::Exchange;
using feed_handler::config::LoadConfigFile;
using feed_handler::config::ParseConfig;
using feed_handler::config::ParseHostPort;

const std::string kKraken = R"(
[[connections]]
id = "kraken-btc"
exchange = "kraken"
env = "prod"
symbols = ["BTC/USD", "ETH/USD"]
api_key_env = "KRAKEN_API_KEY"
api_secret_env = "KRAKEN_API_SECRET"
)";

const std::string kDeribit = R"(
[[connections]]
id = "deribit-perp"
exchange = "deribit"
env = "testnet"
symbols = ["BTC-PERPETUAL"]
api_key_env = "DERIBIT_TESTNET_CLIENT_ID"
api_secret_env = "DERIBIT_TESTNET_CLIENT_SECRET"
)";

/// Asserts that `text` is rejected and that the message mentions `expected`, so a
/// test proves the right rule fired, not merely that something did.
void ExpectRejected(const std::string& text, const std::string& expected) {
    const auto config = ParseConfig(text);
    ASSERT_FALSE(config.has_value()) << "accepted, expected an error mentioning: " << expected;
    EXPECT_NE(config.error().find(expected), std::string::npos) << "error was: " << config.error();
}

/// One connection with `line` swapped in for the named key's line, to vary a
/// single field at a time.
std::string KrakenWith(const std::string& key, const std::string& value_line) {
    std::string text = kKraken;
    const std::size_t begin = text.find("\n" + key + " = ");
    EXPECT_NE(begin, std::string::npos) << key;
    const std::size_t end = text.find('\n', begin + 1);
    text.replace(begin + 1, end - begin - 1, value_line);
    return text;
}

TEST(FeedHandlerConfig, ParsesBothExchangesAndAppliesDefaults) {
    const auto config = ParseConfig(kKraken + kDeribit);
    ASSERT_TRUE(config.has_value()) << config.error();

    EXPECT_EQ(config->journal_dir, "journal");
    EXPECT_EQ(config->state_dir, "state");
    ASSERT_EQ(config->connections.size(), 2U);

    const auto& kraken = config->connections[0];
    EXPECT_EQ(kraken.id, "kraken-btc");
    EXPECT_EQ(kraken.exchange, Exchange::kKraken);
    EXPECT_EQ(kraken.env, Environment::kProd);
    EXPECT_EQ(kraken.feed, "level3");
    EXPECT_EQ(kraken.endpoint, "wss://ws-l3.kraken.com/v2");
    EXPECT_EQ(kraken.symbols, (std::vector<std::string>{"BTC/USD", "ETH/USD"}));
    EXPECT_EQ(kraken.api_key_env, "KRAKEN_API_KEY");
    EXPECT_EQ(kraken.api_secret_env, "KRAKEN_API_SECRET");

    const auto& deribit = config->connections[1];
    EXPECT_EQ(deribit.exchange, Exchange::kDeribit);
    EXPECT_EQ(deribit.env, Environment::kTestnet);
    EXPECT_EQ(deribit.feed, "book");
    EXPECT_EQ(deribit.endpoint, "fix-test.deribit.com:9881");
}

TEST(FeedHandlerConfig, ReadsExplicitDirectoriesAndEndpoints) {
    const auto config = ParseConfig(R"(
journal_dir = "/data/journals"
state_dir = "/data/state"
)" + KrakenWith("env", R"(env = "testnet")") +
                                    "endpoint = \"wss://beta.example.test/v2\"\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->journal_dir, "/data/journals");
    EXPECT_EQ(config->state_dir, "/data/state");
    EXPECT_EQ(config->connections[0].endpoint, "wss://beta.example.test/v2");
}

TEST(FeedHandlerConfig, ConnectionsForKeepsFileOrderPerExchange) {
    const auto config = ParseConfig(kKraken + kDeribit + R"(
[[connections]]
id = "kraken-sol"
exchange = "kraken"
env = "prod"
symbols = ["SOL/USD"]
api_key_env = "KRAKEN_API_KEY"
api_secret_env = "KRAKEN_API_SECRET"
)");
    ASSERT_TRUE(config.has_value()) << config.error();

    const auto kraken = config->ConnectionsFor(Exchange::kKraken);
    ASSERT_EQ(kraken.size(), 2U);
    EXPECT_EQ(kraken[0].id, "kraken-btc");
    EXPECT_EQ(kraken[1].id, "kraken-sol");
    EXPECT_EQ(config->ConnectionsFor(Exchange::kDeribit).size(), 1U);
}

TEST(FeedHandlerConfig, ReportsTomlSyntaxErrorsWithALocation) {
    const auto config = ParseConfig("journal_dir = \n", "my.toml");
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("my.toml:1:"), std::string::npos) << config.error();
}

TEST(FeedHandlerConfig, RejectsUnknownKeysSoATypoCannotSilentlyCaptureNothing) {
    ExpectRejected("journal = \"x\"\n" + kKraken, "unknown key 'journal'");
    // The realistic typo: `symbol` for `symbols`.
    ExpectRejected(KrakenWith("symbols", R"(symbol = ["BTC/USD"])"), "unknown key 'symbol'");
}

TEST(FeedHandlerConfig, RequiresAtLeastOneConnection) {
    ExpectRejected("", "missing required [[connections]]");
    ExpectRejected("connections = []\n", "at least one");
    ExpectRejected("connections = 3\n", "must be an array of tables");
}

TEST(FeedHandlerConfig, RequiresEveryMandatoryKeyAndNamesTheEntry) {
    for (const char* key : {"id", "exchange", "env", "symbols", "api_key_env", "api_secret_env"}) {
        std::string text = kKraken;
        const std::size_t begin = text.find(std::string("\n") + key + " = ");
        ASSERT_NE(begin, std::string::npos) << key;
        text.erase(begin, text.find('\n', begin + 1) - begin);
        ExpectRejected(text, std::string("missing required key '") + key + "'");
    }
    ExpectRejected(KrakenWith("api_key_env", "api_key_env = 3"),
                   "connections[0] (kraken-btc): 'api_key_env' must be a string");
}

TEST(FeedHandlerConfig, ValidatesTheConnectionId) {
    ExpectRejected(KrakenWith("id", R"(id = "Kraken")"), "'id' must be lowercase");
    ExpectRejected(KrakenWith("id", R"(id = "a.b")"), "'id' must be lowercase");
    ExpectRejected(KrakenWith("id", R"(id = "../etc")"), "'id' must be lowercase");
    ExpectRejected(KrakenWith("id", R"(id = "")"), "'id' must be lowercase");
    ExpectRejected(KrakenWith("id", "id = \"" + std::string(49, 'a') + "\""), "'id' must be");
}

TEST(FeedHandlerConfig, RejectsDuplicateIds) {
    ExpectRejected(kKraken + kKraken, "duplicate connection id");
}

TEST(FeedHandlerConfig, RejectsUnknownExchangeAndEnvironment) {
    ExpectRejected(KrakenWith("exchange", R"(exchange = "binance")"), "unknown exchange 'binance'");
    ExpectRejected(KrakenWith("env", R"(env = "staging")"), "unknown env 'staging'");
}

TEST(FeedHandlerConfig, ValidatesSymbols) {
    ExpectRejected(KrakenWith("symbols", "symbols = []"), "'symbols' must not be empty");
    ExpectRejected(KrakenWith("symbols", R"(symbols = "BTC/USD")"), "'symbols' must be an array");
    ExpectRejected(KrakenWith("symbols", "symbols = [1]"), "only strings");
    ExpectRejected(KrakenWith("symbols", R"(symbols = ["BTC/USD", "BTC/USD"])"),
                   "duplicate symbol 'BTC/USD'");
    // Anything that could break out of the hand-built JSON or FIX field.
    ExpectRejected(KrakenWith("symbols", R"(symbols = ["BTC/USD\""])"), "invalid symbol");
    ExpectRejected(KrakenWith("symbols", R"(symbols = ["BTC\\USD"])"), "invalid symbol");
    ExpectRejected(KrakenWith("symbols", R"(symbols = ["BTC USD"])"), "invalid symbol");
    ExpectRejected(KrakenWith("symbols", R"(symbols = ["BTC\u0001USD"])"), "invalid symbol");
    ExpectRejected(KrakenWith("symbols", R"(symbols = [""])"), "invalid symbol");
}

TEST(FeedHandlerConfig, EnforcesKrakensPerConnectionSymbolCap) {
    const auto with_symbols = [](std::size_t count) {
        std::string list = "symbols = [";
        for (std::size_t index = 0; index < count; ++index) {
            list += (index == 0 ? "\"S" : ", \"S") + std::to_string(index) + "\"";
        }
        return KrakenWith("symbols", list + "]");
    };
    EXPECT_TRUE(ParseConfig(with_symbols(200)).has_value());
    ExpectRejected(with_symbols(201), "exceeds Kraken's limit of 200");
}

TEST(FeedHandlerConfig, ChecksTheFeedAgainstWhatTheExchangeSupports) {
    EXPECT_TRUE(
        ParseConfig(KrakenWith("env", R"(env = "prod")") + "feed = \"level3\"\n").has_value());
    ExpectRejected(kKraken + "feed = \"book\"\n", "unsupported feed 'book' for kraken");
    ExpectRejected(kDeribit + "feed = \"level3\"\n", "unsupported feed 'level3' for deribit");
}

TEST(FeedHandlerConfig, RequiresAnEndpointWhereNoDefaultHasBeenVerified) {
    ExpectRejected(KrakenWith("env", R"(env = "testnet")"),
                   "no default endpoint for kraken testnet");
    std::string deribit_prod = kDeribit;
    deribit_prod.replace(deribit_prod.find("testnet"), 7, "prod");
    ExpectRejected(deribit_prod, "no default endpoint for deribit prod");
    EXPECT_TRUE(ParseConfig(deribit_prod + "endpoint = \"www.example.test:9881\"\n").has_value());
}

TEST(FeedHandlerConfig, ValidatesEndpointShape) {
    ExpectRejected(kKraken + "endpoint = \"https://ws-l3.kraken.com/v2\"\n", "invalid endpoint");
    ExpectRejected(kKraken + "endpoint = \"wss://\"\n", "invalid endpoint");
    ExpectRejected(kDeribit + "endpoint = \"fix-test.deribit.com\"\n", "expected host:port");
    ExpectRejected(kDeribit + "endpoint = \"fix-test.deribit.com:0\"\n", "invalid endpoint");
    ExpectRejected(kDeribit + "endpoint = \"fix-test.deribit.com:99999\"\n", "invalid endpoint");
    ExpectRejected(kDeribit + "endpoint = \"host:port\"\n", "invalid endpoint");
}

TEST(FeedHandlerConfig, ParseHostPortSplitsAndBoundsThePort) {
    const auto ok = ParseHostPort("fix-test.deribit.com:9881");
    ASSERT_TRUE(ok.has_value()) << ok.error();
    EXPECT_EQ(ok->host, "fix-test.deribit.com");
    EXPECT_EQ(ok->port, 9881);

    EXPECT_EQ(ParseHostPort("h:1")->port, 1);
    EXPECT_EQ(ParseHostPort("h:65535")->port, 65535);
    for (const char* bad : {"h:0", "h:65536", "h:", ":1", "h", "h:1x", "h:-1", "h:123456", "a b:1"}) {
        EXPECT_FALSE(ParseHostPort(bad).has_value()) << bad;
    }
}

TEST(FeedHandlerConfig, CredentialFieldsMustBeEnvironmentVariableNames) {
    // The mistake this guards: pasting the secret where its variable name goes.
    ExpectRejected(KrakenWith("api_secret_env", R"(api_secret_env = "abc+/def==")"),
                   "NAME of an environment variable");
    ExpectRejected(KrakenWith("api_secret_env", R"(api_secret_env = "1ABC")"),
                   "NAME of an environment variable");
    ExpectRejected(KrakenWith("api_secret_env", R"(api_secret_env = "KRAKEN_API_KEY")"),
                   "name the same variable");
}

TEST(FeedHandlerConfig, RejectsAnEmptyDirectory) {
    ExpectRejected("journal_dir = \"\"\n" + kKraken, "'journal_dir' must not be empty");
}

TEST(FeedHandlerConfig, LoadConfigFileNamesThePathOnFailure) {
    const auto config = LoadConfigFile("/nonexistent/feed_handler.toml");
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("/nonexistent/feed_handler.toml"), std::string::npos);
}

TEST(FeedHandlerConfig, TheCommittedDefaultConfigIsValid) {
    const auto config =
        LoadConfigFile(std::filesystem::path(FEED_HANDLER_CONFIG_DIR) / "feed_handler.toml");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->ConnectionsFor(Exchange::kKraken).size(), 1U);
    EXPECT_EQ(config->ConnectionsFor(Exchange::kDeribit).size(), 1U);
}

TEST(FeedHandlerConfigArgs, AcceptsBothFlagSpellings) {
    const std::array<const char*, 3> spaced = {"prog", "--config", "a.toml"};
    const auto first = ConfigPathFromArgs(static_cast<int>(spaced.size()), spaced.data());
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_EQ(*first, "a.toml");

    const std::array<const char*, 2> joined = {"prog", "--config=b.toml"};
    const auto second = ConfigPathFromArgs(static_cast<int>(joined.size()), joined.data());
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ(*second, "b.toml");
}

TEST(FeedHandlerConfigArgs, RejectsEverythingElseWithTheUsageLine) {
    const std::array<const char*, 1> none = {"prog"};
    const std::array<const char*, 2> dangling = {"prog", "--config"};
    const std::array<const char*, 2> empty_value = {"prog", "--config="};
    const std::array<const char*, 2> unknown = {"prog", "--verbose"};
    const std::array<const char*, 5> twice = {"prog", "--config", "a", "--config", "b"};

    const auto expect_usage = [](const auto& args) {
        const auto path = ConfigPathFromArgs(static_cast<int>(args.size()), args.data());
        ASSERT_FALSE(path.has_value());
        EXPECT_EQ(path.error(), "usage: prog --config <path>");
    };
    expect_usage(none);
    expect_usage(dangling);
    expect_usage(empty_value);
    expect_usage(unknown);
    expect_usage(twice);
}

}  // namespace
