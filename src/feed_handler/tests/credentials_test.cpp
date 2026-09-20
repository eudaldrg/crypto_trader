#include "feed_handler/credentials.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {

using feed_handler::Credential;
using feed_handler::EnvReader;
using feed_handler::ResolveCredentials;
using feed_handler::config::Connection;
using feed_handler::config::ParseConfig;

// Distinctive fakes, so a test can prove a value never reaches an error string.
constexpr const char* kKrakenKeyValue = "fake-kraken-key-value-7f3a";
constexpr const char* kKrakenSecretValue = "fake-kraken-secret-value-91bc";

/// A fake environment; never touches the real one.
EnvReader FakeEnv(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(std::string(name));
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

std::vector<Connection> Connections() {
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
)");
    EXPECT_TRUE(config.has_value());
    return config->connections;
}

TEST(FeedHandlerCredentials, ReturnsOneCredentialPerConnectionInOrder) {
    const auto resolved = ResolveCredentials(
        Connections(),
        FakeEnv({{"K_KEY", "k1"}, {"K_SECRET", "k2"}, {"D_KEY", "d1"}, {"D_SECRET", "d2"}}));
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    ASSERT_EQ(resolved->size(), 2U);
    EXPECT_EQ((*resolved)[0].key, "k1");
    EXPECT_EQ((*resolved)[0].secret, "k2");
    EXPECT_EQ((*resolved)[1].key, "d1");
    EXPECT_EQ((*resolved)[1].secret, "d2");
}

TEST(FeedHandlerCredentials, AMissingVariableIsNamedWithItsConnectionId) {
    const auto resolved = ResolveCredentials(
        Connections(), FakeEnv({{"K_KEY", "k1"}, {"K_SECRET", "k2"}, {"D_KEY", "d1"}}));
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("[deribit-a] D_SECRET"), std::string::npos) << resolved.error();
    EXPECT_EQ(resolved.error().find("K_KEY"), std::string::npos) << "named a variable that was set";
}

TEST(FeedHandlerCredentials, BothVariablesOfAConnectionAreNamedWhenBothAreMissing) {
    const auto resolved =
        ResolveCredentials(Connections(), FakeEnv({{"K_KEY", "k1"}, {"K_SECRET", "k2"}}));
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("[deribit-a] D_KEY"), std::string::npos) << resolved.error();
    EXPECT_NE(resolved.error().find("[deribit-a] D_SECRET"), std::string::npos) << resolved.error();
}

TEST(FeedHandlerCredentials, EveryMissingVariableAcrossAllConnectionsIsReportedAtOnce) {
    const auto resolved = ResolveCredentials(Connections(), FakeEnv({}));
    ASSERT_FALSE(resolved.has_value());
    for (const char* expected :
         {"[kraken-a] K_KEY", "[kraken-a] K_SECRET", "[deribit-a] D_KEY", "[deribit-a] D_SECRET"}) {
        EXPECT_NE(resolved.error().find(expected), std::string::npos)
            << expected << " missing from: " << resolved.error();
    }
}

TEST(FeedHandlerCredentials, AnEmptyValueCountsAsMissing) {
    const auto resolved = ResolveCredentials(
        Connections(),
        FakeEnv({{"K_KEY", ""}, {"K_SECRET", "k2"}, {"D_KEY", "d1"}, {"D_SECRET", "d2"}}));
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("[kraken-a] K_KEY"), std::string::npos) << resolved.error();
}

TEST(FeedHandlerCredentials, TheErrorNeverContainsAValueThatWasSet) {
    // K_KEY and K_SECRET are set to distinctive values; D_* are missing so an
    // error is produced. Neither value may appear in it.
    const auto resolved = ResolveCredentials(
        Connections(), FakeEnv({{"K_KEY", kKrakenKeyValue}, {"K_SECRET", kKrakenSecretValue}}));
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(resolved.error().find(kKrakenKeyValue), std::string::npos) << resolved.error();
    EXPECT_EQ(resolved.error().find(kKrakenSecretValue), std::string::npos) << resolved.error();
}

TEST(FeedHandlerCredentials, ANoConnectionsListResolvesToNothing) {
    const auto resolved = ResolveCredentials({}, FakeEnv({}));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->empty());
}

TEST(FeedHandlerCredentials, SystemEnvReadsTheProcessEnvironment) {
    const EnvReader env = feed_handler::SystemEnv();
    // PATH is set in every environment this test runs in, and an implausible
    // name is not.
    EXPECT_TRUE(env("PATH").has_value());
    EXPECT_FALSE(env("CRYPTO_TRADER_SURELY_UNSET_VARIABLE_0F3A").has_value());
}

}  // namespace
