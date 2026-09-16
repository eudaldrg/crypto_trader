// Kraken REST signing/nonce/reference-data logic. Everything here is offline:
// the live-network path (an actual GetWebSocketsToken call) is deliberately
// not a checked-in test, since it needs real credentials.
// See exchanges/kraken.md, "Auth".
#include "feed_handler/kraken/kraken_rest_client.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "feed_handler/kraken/kraken_signing.h"

namespace {

using feed_handler::kraken::Base64Decode;
using feed_handler::kraken::Base64Encode;
using feed_handler::kraken::EncodePostData;
using feed_handler::kraken::NonceGenerator;
using feed_handler::kraken::PersistentNonceSource;
using feed_handler::kraken::SignPrivateRequest;
using feed_handler::kraken::UrlEncode;

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// A throwaway secret that has never been a real key: base64 of the ASCII
// "kraken-test-secret-do-not-use-0123456789".
constexpr std::string_view kTestSecret = "a3Jha2VuLXRlc3Qtc2VjcmV0LWRvLW5vdC11c2UtMDEyMzQ1Njc4OQ==";
constexpr std::string_view kTokenPath = "/0/private/GetWebSocketsToken";

// Known answer produced by an independent implementation of the same scheme
// (Python hashlib/hmac, exactly as experiments/kraken_l3_probe.py signs), so
// this pins the algorithm rather than just this code's self-consistency.
constexpr std::string_view kExpectedSignature =
    "g4suuu4djNxLp6Txqx6r5BFwS3uPPOExtw2kJqSeICqn6Wz+nG9SCT+y1RYWItviGMHVkHG8KluDaMd1kK4imQ==";

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

// Captured verbatim from GET /0/public/AssetPairs?pair=XBTUSD on 2026-09-16,
// trimmed to the fields this client reads.
constexpr std::string_view kAssetPairsBody =
    R"({"error":[],"result":{)"
    R"("XXBTZUSD":{"altname":"XBTUSD","wsname":"XBT/USD","aclass_base":"currency",)"
    R"("base":"XXBT","aclass_quote":"currency","quote":"ZUSD","lot":"unit",)"
    R"("cost_decimals":5,"pair_decimals":1,"lot_decimals":8,"lot_multiplier":1,)"
    R"("ordermin":"0.00005","costmin":"0.5","tick_size":"0.1","status":"online"},)"
    R"("XETHZUSD":{"altname":"ETHUSD","wsname":"ETH/USD","aclass_base":"currency",)"
    R"("base":"XETH","aclass_quote":"currency","quote":"ZUSD","lot":"unit",)"
    R"("cost_decimals":5,"pair_decimals":2,"lot_decimals":8,"lot_multiplier":1,)"
    R"("ordermin":"0.002","costmin":"0.5","tick_size":"0.01","status":"online"})"
    R"(}})";

}  // namespace

// The tests live at namespace scope rather than inside the anonymous namespace
// above: cppcheck cannot parse GoogleTest's TEST macros when they follow
// another definition inside an anonymous namespace, and the pre-commit
// cppcheck hook treats that as a hard error.

TEST(KrakenUrlEncode, MatchesPythonUrlencodeConventions) {
    // Python's urllib.parse.urlencode is what the probe signs with, so the
    // C++ encoder has to agree with it byte for byte or the signature will
    // cover a different body than the one sent.
    EXPECT_EQ(UrlEncode("1700000000000000"), "1700000000000000");
    EXPECT_EQ(UrlEncode("BTC/USD"), "BTC%2FUSD");
    EXPECT_EQ(UrlEncode("a b+c~d"), "a+b%2Bc~d");
    EXPECT_EQ(UrlEncode("-_.~"), "-_.~");
    EXPECT_EQ(UrlEncode(""), "");
}

TEST(KrakenPostData, PreservesParameterOrder) {
    const std::array<std::pair<std::string, std::string>, 2> params = {
        std::pair<std::string, std::string>{"nonce", "1700000000000000"},
        std::pair<std::string, std::string>{"pair", "BTC/USD"},
    };
    EXPECT_EQ(EncodePostData(params), "nonce=1700000000000000&pair=BTC%2FUSD");
}

TEST(KrakenBase64, RoundTripsAndRecoversExactLength) {
    for (std::string_view sample : {"k", "kr", "kra", "krak", "kraken-secret-bytes"}) {
        const std::string encoded = Base64Encode(BytesOf(sample));
        const auto decoded = Base64Decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        ASSERT_EQ(decoded->size(), sample.size()) << "sample: " << sample;
        EXPECT_EQ(std::string(std::bit_cast<const char*>(decoded->data()), decoded->size()),
                  sample);
    }
}

TEST(KrakenBase64, RejectsMalformedInput) {
    EXPECT_FALSE(Base64Decode("").has_value());
    EXPECT_FALSE(Base64Decode("abc").has_value());
    EXPECT_FALSE(Base64Decode("!!!!").has_value());
}

TEST(KrakenSigning, MatchesIndependentReferenceImplementation) {
    const auto signature =
        SignPrivateRequest(kTokenPath, "1700000000000000", "nonce=1700000000000000", kTestSecret);
    ASSERT_TRUE(signature.has_value()) << signature.error();
    EXPECT_EQ(*signature, kExpectedSignature);
}

TEST(KrakenSigning, ProducesAWellShapedHmacSha512Signature) {
    const auto signature =
        SignPrivateRequest(kTokenPath, "1700000000000000", "nonce=1700000000000000", kTestSecret);
    ASSERT_TRUE(signature.has_value()) << signature.error();

    // base64 of a 64-byte HMAC-SHA512 digest is always 88 characters with a
    // single '=' of padding.
    EXPECT_EQ(signature->size(), 88U);
    EXPECT_EQ(signature->back(), '=');
    const auto decoded = Base64Decode(*signature);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 64U);
}

TEST(KrakenSigning, IsDeterministicForIdenticalInput) {
    const auto first = SignPrivateRequest(kTokenPath, "42", "nonce=42", kTestSecret);
    const auto second = SignPrivateRequest(kTokenPath, "42", "nonce=42", kTestSecret);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
}

TEST(KrakenSigning, EveryInputComponentChangesTheSignature) {
    const auto base = SignPrivateRequest(kTokenPath, "42", "nonce=42", kTestSecret);
    ASSERT_TRUE(base.has_value());

    // The url path is part of the signed message, so the same nonce/body
    // signed for a different endpoint must not produce the same signature.
    const auto other_path = SignPrivateRequest("/0/private/Balance", "42", "nonce=42", kTestSecret);
    const auto other_nonce = SignPrivateRequest(kTokenPath, "43", "nonce=43", kTestSecret);
    const auto other_secret = SignPrivateRequest(
        kTokenPath, "42", "nonce=42", "b3RoZXItc2VjcmV0LWRvLW5vdC11c2UtMDEyMzQ1Njc4OQ==");
    ASSERT_TRUE(other_path.has_value());
    ASSERT_TRUE(other_nonce.has_value());
    ASSERT_TRUE(other_secret.has_value());

    EXPECT_NE(*base, *other_path);
    EXPECT_NE(*base, *other_nonce);
    EXPECT_NE(*base, *other_secret);
}

TEST(KrakenSigning, FailsCleanlyOnANonBase64Secret) {
    const auto signature = SignPrivateRequest(kTokenPath, "42", "nonce=42", "not base64!");
    ASSERT_FALSE(signature.has_value());
    // The failure must never quote the secret back.
    EXPECT_EQ(signature.error().find("not base64!"), std::string::npos);
}

TEST(KrakenNonce, IsStrictlyIncreasingUnderRapidRepeatedCalls) {
    NonceGenerator nonce;
    std::uint64_t previous = 0;
    std::set<std::uint64_t> seen;
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const std::uint64_t value = nonce.Next();
        EXPECT_GT(value, previous);
        seen.insert(value);
        previous = value;
    }
    EXPECT_EQ(seen.size(), 10000U);
}

TEST(KrakenNonce, AdvancesWhenTheClockDoesNot) {
    // The exact failure mode a raw millisecond timestamp has: several calls
    // landing inside one clock tick (exchanges/kraken.md).
    NonceGenerator nonce;
    EXPECT_EQ(nonce.NextFrom(1'700'000'000'000'000ULL), 1'700'000'000'000'000ULL);
    EXPECT_EQ(nonce.NextFrom(1'700'000'000'000'000ULL), 1'700'000'000'000'001ULL);
    EXPECT_EQ(nonce.NextFrom(1'700'000'000'000'000ULL), 1'700'000'000'000'002ULL);
    // A later real timestamp wins again once the clock catches up.
    EXPECT_EQ(nonce.NextFrom(1'700'000'000'000'010ULL), 1'700'000'000'000'010ULL);
}

TEST(KrakenNonce, SurvivesBackwardsClockSkew) {
    NonceGenerator nonce;
    EXPECT_EQ(nonce.NextFrom(1'700'000'000'000'000ULL), 1'700'000'000'000'000ULL);
    // NTP steps the clock back a full second: the nonce must still increase,
    // because Kraken rejects a non-increasing one for the whole API key.
    EXPECT_EQ(nonce.NextFrom(1'699'999'999'000'000ULL), 1'700'000'000'000'001ULL);
    EXPECT_EQ(nonce.NextFrom(0), 1'700'000'000'000'002ULL);
}

TEST(KrakenAssetPairs, ParsesTickSizeAndDecimals) {
    feed_handler::kraken::RestClient client;
    const auto count = client.ParseAssetPairs(kAssetPairsBody);
    ASSERT_TRUE(count.has_value()) << count.error();
    EXPECT_EQ(*count, 2U);
    EXPECT_EQ(client.CachedPairCount(), 2U);

    const auto* btc = client.FindAssetPair("XXBTZUSD");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->altname, "XBTUSD");
    EXPECT_EQ(btc->ws_name, "XBT/USD");
    EXPECT_EQ(btc->price_decimals, 1);
    EXPECT_EQ(btc->qty_decimals, 8);
    EXPECT_EQ(btc->tick_size, "0.1");
    EXPECT_DOUBLE_EQ(btc->tick_size_value, 0.1);

    const auto* eth = client.FindAssetPair("ETH/USD");
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(eth->rest_name, "XETHZUSD");
    EXPECT_EQ(eth->price_decimals, 2);
    EXPECT_DOUBLE_EQ(eth->tick_size_value, 0.01);
}

TEST(KrakenAssetPairs, ResolvesTheWsV2SpellingOfBitcoin) {
    // The discrepancy that would otherwise bite at subscribe time: REST
    // reference data says "XBT/USD", WS v2 says "BTC/USD".
    feed_handler::kraken::RestClient client;
    ASSERT_TRUE(client.ParseAssetPairs(kAssetPairsBody).has_value());

    const auto* by_rest = client.FindAssetPair("XXBTZUSD");
    const auto* by_ws_name = client.FindAssetPair("XBT/USD");
    const auto* by_ws_v2_name = client.FindAssetPair("BTC/USD");
    const auto* by_altname = client.FindAssetPair("XBTUSD");
    ASSERT_NE(by_rest, nullptr);
    EXPECT_EQ(by_ws_name, by_rest);
    EXPECT_EQ(by_ws_v2_name, by_rest);
    EXPECT_EQ(by_altname, by_rest);

    EXPECT_EQ(client.FindAssetPair("DOGE/MOON"), nullptr);
}

TEST(KrakenAssetPairs, SurfacesKrakenErrorArrays) {
    feed_handler::kraken::RestClient client;
    const auto parsed =
        client.ParseAssetPairs(R"({"error":["EQuery:Unknown asset pair"],"result":{}})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("EQuery:Unknown asset pair"), std::string::npos);
}

TEST(KrakenAssetPairs, RejectsGarbageBody) {
    feed_handler::kraken::RestClient client;
    EXPECT_FALSE(client.ParseAssetPairs("this is not json").has_value());
}

TEST(KrakenCredentials, RefusesToSignWithoutCredentials) {
    feed_handler::kraken::RestClient client;
    const auto result = client.FetchWebsocketsToken({});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not set"), std::string::npos);
}

TEST(KrakenNonce, DefaultConstructedPersistentSourceBehavesLikeTheBareGenerator) {
    // Backward compatibility: anything not opting into a state file must get
    // exactly today's in-memory behavior, and must touch no files at all.
    PersistentNonceSource source;
    NonceGenerator reference;
    EXPECT_FALSE(source.Persisting());
    EXPECT_TRUE(source.StateFile().empty());
    EXPECT_EQ(source.SeededFrom(), 0U);

    for (const std::uint64_t now : {1'700'000'000'000'000ULL, 1'700'000'000'000'000ULL,
                                    1'699'999'999'000'000ULL, 1'700'000'000'000'010ULL, 0ULL}) {
        EXPECT_EQ(source.NextFrom(now), reference.NextFrom(now));
    }
    EXPECT_EQ(source.Last(), reference.Last());
}

/// Gives each test its own directory under the system temp dir, so a state
/// file written by one test cannot seed another.
class KrakenNonceState : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("kraken_nonce_" +
                      std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "kraken-nonce.state";
    }

    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST_F(KrakenNonceState, SurvivesARestartAtTheSameFile) {
    constexpr std::uint64_t kNow = 1'700'000'000'000'000ULL;

    std::uint64_t last_before_restart = 0;
    {
        PersistentNonceSource source(path_);
        EXPECT_TRUE(source.Persisting());
        EXPECT_EQ(source.SeededFrom(), 0U);
        source.NextFrom(kNow);
        source.NextFrom(kNow);
        last_before_restart = source.NextFrom(kNow);
    }
    EXPECT_EQ(last_before_restart, kNow + 2);
    EXPECT_TRUE(std::filesystem::exists(path_));
    EXPECT_EQ(ReadTextFile(path_), std::to_string(last_before_restart) + "\n");

    // The restart: a fresh process, same file, and a clock that has not moved.
    PersistentNonceSource restarted(path_);
    EXPECT_EQ(restarted.SeededFrom(), last_before_restart);
    EXPECT_EQ(restarted.NextFrom(kNow), last_before_restart + 1);
    EXPECT_EQ(restarted.NextFrom(kNow), last_before_restart + 2);
}

TEST_F(KrakenNonceState, DoesNotRegressWhenThePersistedMarkIsInTheFuture) {
    // The case the file exists for: the clock stepped backwards while the
    // process was down, so "now" is well behind what Kraken has already seen.
    constexpr std::uint64_t kPersisted = 1'700'000'000'000'000ULL;
    constexpr std::uint64_t kRewoundNow = 1'699'999'000'000'000ULL;
    WriteTextFile(path_, std::to_string(kPersisted) + "\n");

    PersistentNonceSource source(path_);
    EXPECT_EQ(source.SeededFrom(), kPersisted);
    EXPECT_EQ(source.NextFrom(kRewoundNow), kPersisted + 1);
    EXPECT_EQ(source.NextFrom(kRewoundNow), kPersisted + 2);
    // And the clock wins again as soon as it has genuinely caught up.
    EXPECT_EQ(source.NextFrom(kPersisted + 500), kPersisted + 500);
}

TEST_F(KrakenNonceState, FallsBackToTheClockWhenTheFileIsMissing) {
    constexpr std::uint64_t kNow = 1'700'000'000'000'000ULL;
    ASSERT_FALSE(std::filesystem::exists(path_));

    // First run: nothing to restore, so this is plain clock behavior, and the
    // mark starts being recorded from here.
    PersistentNonceSource source(path_);
    EXPECT_EQ(source.SeededFrom(), 0U);
    EXPECT_EQ(source.NextFrom(kNow), kNow);
    EXPECT_EQ(ReadTextFile(path_), std::to_string(kNow) + "\n");
}

TEST_F(KrakenNonceState, FallsBackToTheClockOnAGarbageFile) {
    constexpr std::uint64_t kNow = 1'700'000'000'000'000ULL;
    for (const std::string_view contents :
         {"", "   \n", "not-a-nonce", "1700000000000000 1700000000000001", "-5", "1e6",
          "1700000000000000garbage"}) {
        WriteTextFile(path_, contents);

        PersistentNonceSource source(path_);
        EXPECT_EQ(source.SeededFrom(), 0U) << "contents: " << contents;
        EXPECT_EQ(source.NextFrom(kNow), kNow) << "contents: " << contents;
        // The unusable contents are replaced rather than left to be re-read.
        EXPECT_EQ(ReadTextFile(path_), std::to_string(kNow) + "\n");
    }
}

TEST_F(KrakenNonceState, ToleratesAnUnwritableLocation) {
    // The parent "directory" is a regular file, so neither create_directories
    // nor the write can ever succeed -- which must still leave a perfectly
    // usable clock-only nonce source rather than a failed startup.
    constexpr std::uint64_t kNow = 1'700'000'000'000'000ULL;
    const std::filesystem::path blocker = directory_ / "not-a-directory";
    WriteTextFile(blocker, "occupied");
    const std::filesystem::path unwritable = blocker / "kraken-nonce.state";

    PersistentNonceSource source(unwritable);
    EXPECT_TRUE(source.Persisting());
    EXPECT_EQ(source.SeededFrom(), 0U);
    EXPECT_EQ(source.NextFrom(kNow), kNow);
    EXPECT_EQ(source.NextFrom(kNow), kNow + 1);
    EXPECT_FALSE(std::filesystem::is_directory(blocker));
}

TEST_F(KrakenNonceState, RestClientOptsInWithoutChangingTheDefault) {
    // The wiring the binary uses: a path on the RestClient seeds the same
    // source, while the default constructor stays purely in-memory.
    WriteTextFile(path_, "1700000000000000\n");
    const feed_handler::kraken::RestClient persisted(
        std::string(feed_handler::kraken::RestClient::kDefaultBaseUrl), path_);
    EXPECT_TRUE(persisted.NonceSource().Persisting());
    EXPECT_EQ(persisted.NonceSource().SeededFrom(), 1'700'000'000'000'000ULL);

    const feed_handler::kraken::RestClient plain;
    EXPECT_FALSE(plain.NonceSource().Persisting());
    EXPECT_EQ(plain.NonceSource().SeededFrom(), 0U);
}
