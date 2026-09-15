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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "feed_handler/kraken/kraken_signing.h"

namespace {

using feed_handler::kraken::base64_decode;
using feed_handler::kraken::base64_encode;
using feed_handler::kraken::encode_post_data;
using feed_handler::kraken::nonce_generator;
using feed_handler::kraken::sign_private_request;
using feed_handler::kraken::url_encode;

// A throwaway secret that has never been a real key: base64 of the ASCII
// "kraken-test-secret-do-not-use-0123456789".
constexpr std::string_view kTestSecret = "a3Jha2VuLXRlc3Qtc2VjcmV0LWRvLW5vdC11c2UtMDEyMzQ1Njc4OQ==";
constexpr std::string_view kTokenPath = "/0/private/GetWebSocketsToken";

// Known answer produced by an independent implementation of the same scheme
// (Python hashlib/hmac, exactly as experiments/kraken_l3_probe.py signs), so
// this pins the algorithm rather than just this code's self-consistency.
constexpr std::string_view kExpectedSignature =
    "g4suuu4djNxLp6Txqx6r5BFwS3uPPOExtw2kJqSeICqn6Wz+nG9SCT+y1RYWItviGMHVkHG8KluDaMd1kK4imQ==";

std::span<const std::byte> bytes_of(std::string_view text) {
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
    EXPECT_EQ(url_encode("1700000000000000"), "1700000000000000");
    EXPECT_EQ(url_encode("BTC/USD"), "BTC%2FUSD");
    EXPECT_EQ(url_encode("a b+c~d"), "a+b%2Bc~d");
    EXPECT_EQ(url_encode("-_.~"), "-_.~");
    EXPECT_EQ(url_encode(""), "");
}

TEST(KrakenPostData, PreservesParameterOrder) {
    const std::array<std::pair<std::string, std::string>, 2> params = {
        std::pair<std::string, std::string>{"nonce", "1700000000000000"},
        std::pair<std::string, std::string>{"pair", "BTC/USD"},
    };
    EXPECT_EQ(encode_post_data(params), "nonce=1700000000000000&pair=BTC%2FUSD");
}

TEST(KrakenBase64, RoundTripsAndRecoversExactLength) {
    for (std::string_view sample : {"k", "kr", "kra", "krak", "kraken-secret-bytes"}) {
        const std::string encoded = base64_encode(bytes_of(sample));
        const auto decoded = base64_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        ASSERT_EQ(decoded->size(), sample.size()) << "sample: " << sample;
        EXPECT_EQ(std::string(std::bit_cast<const char*>(decoded->data()), decoded->size()),
                  sample);
    }
}

TEST(KrakenBase64, RejectsMalformedInput) {
    EXPECT_FALSE(base64_decode("").has_value());
    EXPECT_FALSE(base64_decode("abc").has_value());
    EXPECT_FALSE(base64_decode("!!!!").has_value());
}

TEST(KrakenSigning, MatchesIndependentReferenceImplementation) {
    const auto signature =
        sign_private_request(kTokenPath, "1700000000000000", "nonce=1700000000000000", kTestSecret);
    ASSERT_TRUE(signature.has_value()) << signature.error();
    EXPECT_EQ(*signature, kExpectedSignature);
}

TEST(KrakenSigning, ProducesAWellShapedHmacSha512Signature) {
    const auto signature =
        sign_private_request(kTokenPath, "1700000000000000", "nonce=1700000000000000", kTestSecret);
    ASSERT_TRUE(signature.has_value()) << signature.error();

    // base64 of a 64-byte HMAC-SHA512 digest is always 88 characters with a
    // single '=' of padding.
    EXPECT_EQ(signature->size(), 88U);
    EXPECT_EQ(signature->back(), '=');
    const auto decoded = base64_decode(*signature);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 64U);
}

TEST(KrakenSigning, IsDeterministicForIdenticalInput) {
    const auto first = sign_private_request(kTokenPath, "42", "nonce=42", kTestSecret);
    const auto second = sign_private_request(kTokenPath, "42", "nonce=42", kTestSecret);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
}

TEST(KrakenSigning, EveryInputComponentChangesTheSignature) {
    const auto base = sign_private_request(kTokenPath, "42", "nonce=42", kTestSecret);
    ASSERT_TRUE(base.has_value());

    // The url path is part of the signed message, so the same nonce/body
    // signed for a different endpoint must not produce the same signature.
    const auto other_path =
        sign_private_request("/0/private/Balance", "42", "nonce=42", kTestSecret);
    const auto other_nonce = sign_private_request(kTokenPath, "43", "nonce=43", kTestSecret);
    const auto other_secret = sign_private_request(
        kTokenPath, "42", "nonce=42", "b3RoZXItc2VjcmV0LWRvLW5vdC11c2UtMDEyMzQ1Njc4OQ==");
    ASSERT_TRUE(other_path.has_value());
    ASSERT_TRUE(other_nonce.has_value());
    ASSERT_TRUE(other_secret.has_value());

    EXPECT_NE(*base, *other_path);
    EXPECT_NE(*base, *other_nonce);
    EXPECT_NE(*base, *other_secret);
}

TEST(KrakenSigning, FailsCleanlyOnANonBase64Secret) {
    const auto signature = sign_private_request(kTokenPath, "42", "nonce=42", "not base64!");
    ASSERT_FALSE(signature.has_value());
    // The failure must never quote the secret back.
    EXPECT_EQ(signature.error().find("not base64!"), std::string::npos);
}

TEST(KrakenNonce, IsStrictlyIncreasingUnderRapidRepeatedCalls) {
    nonce_generator nonce;
    std::uint64_t previous = 0;
    std::set<std::uint64_t> seen;
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const std::uint64_t value = nonce.next();
        EXPECT_GT(value, previous);
        seen.insert(value);
        previous = value;
    }
    EXPECT_EQ(seen.size(), 10000U);
}

TEST(KrakenNonce, AdvancesWhenTheClockDoesNot) {
    // The exact failure mode a raw millisecond timestamp has: several calls
    // landing inside one clock tick (exchanges/kraken.md).
    nonce_generator nonce;
    EXPECT_EQ(nonce.next_from(1'700'000'000'000'000ULL), 1'700'000'000'000'000ULL);
    EXPECT_EQ(nonce.next_from(1'700'000'000'000'000ULL), 1'700'000'000'000'001ULL);
    EXPECT_EQ(nonce.next_from(1'700'000'000'000'000ULL), 1'700'000'000'000'002ULL);
    // A later real timestamp wins again once the clock catches up.
    EXPECT_EQ(nonce.next_from(1'700'000'000'000'010ULL), 1'700'000'000'000'010ULL);
}

TEST(KrakenNonce, SurvivesBackwardsClockSkew) {
    nonce_generator nonce;
    EXPECT_EQ(nonce.next_from(1'700'000'000'000'000ULL), 1'700'000'000'000'000ULL);
    // NTP steps the clock back a full second: the nonce must still increase,
    // because Kraken rejects a non-increasing one for the whole API key.
    EXPECT_EQ(nonce.next_from(1'699'999'999'000'000ULL), 1'700'000'000'000'001ULL);
    EXPECT_EQ(nonce.next_from(0), 1'700'000'000'000'002ULL);
}

TEST(KrakenAssetPairs, ParsesTickSizeAndDecimals) {
    feed_handler::kraken::rest_client client;
    const auto count = client.parse_asset_pairs(kAssetPairsBody);
    ASSERT_TRUE(count.has_value()) << count.error();
    EXPECT_EQ(*count, 2U);
    EXPECT_EQ(client.cached_pair_count(), 2U);

    const auto* btc = client.find_asset_pair("XXBTZUSD");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->altname, "XBTUSD");
    EXPECT_EQ(btc->ws_name, "XBT/USD");
    EXPECT_EQ(btc->price_decimals, 1);
    EXPECT_EQ(btc->qty_decimals, 8);
    EXPECT_EQ(btc->tick_size, "0.1");
    EXPECT_DOUBLE_EQ(btc->tick_size_value, 0.1);

    const auto* eth = client.find_asset_pair("ETH/USD");
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(eth->rest_name, "XETHZUSD");
    EXPECT_EQ(eth->price_decimals, 2);
    EXPECT_DOUBLE_EQ(eth->tick_size_value, 0.01);
}

TEST(KrakenAssetPairs, ResolvesTheWsV2SpellingOfBitcoin) {
    // The discrepancy that would otherwise bite at subscribe time: REST
    // reference data says "XBT/USD", WS v2 says "BTC/USD".
    feed_handler::kraken::rest_client client;
    ASSERT_TRUE(client.parse_asset_pairs(kAssetPairsBody).has_value());

    const auto* by_rest = client.find_asset_pair("XXBTZUSD");
    const auto* by_ws_name = client.find_asset_pair("XBT/USD");
    const auto* by_ws_v2_name = client.find_asset_pair("BTC/USD");
    const auto* by_altname = client.find_asset_pair("XBTUSD");
    ASSERT_NE(by_rest, nullptr);
    EXPECT_EQ(by_ws_name, by_rest);
    EXPECT_EQ(by_ws_v2_name, by_rest);
    EXPECT_EQ(by_altname, by_rest);

    EXPECT_EQ(client.find_asset_pair("DOGE/MOON"), nullptr);
}

TEST(KrakenAssetPairs, SurfacesKrakenErrorArrays) {
    feed_handler::kraken::rest_client client;
    const auto parsed =
        client.parse_asset_pairs(R"({"error":["EQuery:Unknown asset pair"],"result":{}})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("EQuery:Unknown asset pair"), std::string::npos);
}

TEST(KrakenAssetPairs, RejectsGarbageBody) {
    feed_handler::kraken::rest_client client;
    EXPECT_FALSE(client.parse_asset_pairs("this is not json").has_value());
}

TEST(KrakenCredentials, RefusesToSignWithoutCredentials) {
    feed_handler::kraken::rest_client client;
    const auto result = client.fetch_websockets_token({});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not set"), std::string::npos);
}
