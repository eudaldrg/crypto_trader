// Helpers shared by the feed-handler tests that build configs, journals and
// timings. Header-only and namespaced apart from gtest's own `testing`.
#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"

namespace feed_handler::test_support {

/// Fake values, not credentials; the secret is base64 of "fake-secret", which is
/// what Kraken's signing decodes.
inline const Credential kFakeCredential{.key = "fake-key-not-a-credential",
                                        .secret = "ZmFrZS1zZWNyZXQ="};

/// One Kraken `[[connections]]` entry, with `extra` (whole TOML lines, each
/// ending in a newline) appended, e.g. an `endpoint`.
inline std::string KrakenEntry(std::string_view id, std::string_view symbol = "BTC/USD",
                               std::string_view extra = "") {
    return "\n[[connections]]\nid = \"" + std::string(id) +
           "\"\nexchange = \"kraken\"\nenv = \"prod\"\nsymbols = [\"" + std::string(symbol) +
           "\"]\napi_key_env = \"K_KEY\"\napi_secret_env = \"K_SECRET\"\n" + std::string(extra);
}

/// One Deribit testnet `[[connections]]` entry; see KrakenEntry.
inline std::string DeribitEntry(std::string_view id, std::string_view symbol = "BTC-PERPETUAL",
                                std::string_view extra = "") {
    return "\n[[connections]]\nid = \"" + std::string(id) +
           "\"\nexchange = \"deribit\"\nenv = \"testnet\"\nsymbols = [\"" + std::string(symbol) +
           "\"]\napi_key_env = \"D_KEY\"\napi_secret_env = \"D_SECRET\"\n" + std::string(extra);
}

/// Parses `toml`, failing the calling test (and returning an empty config) when
/// it is invalid.
inline config::FeedHandlerConfig MustParse(const std::string& toml) {
    auto parsed = config::ParseConfig(toml);
    if (!parsed) {
        ADD_FAILURE() << "test config did not parse: " << parsed.error();
        return {};
    }
    return std::move(*parsed);
}

/// A path under the temp directory unique to the running test and process, with
/// anything already there removed. The caller removes it in TearDown.
inline std::filesystem::path UniqueTestDir(std::string_view prefix) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string(prefix) + "_" +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
                      std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    return dir;
}

inline std::chrono::milliseconds ElapsedSince(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 since);
}

}  // namespace feed_handler::test_support
