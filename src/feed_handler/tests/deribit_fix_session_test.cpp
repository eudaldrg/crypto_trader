// Deribit FIX session mechanics. Everything here is offline: a live testnet
// Logon needs real credentials and a socket, neither of which belongs in a
// checked-in test (same split as kraken_rest_client_test.cpp).
//
// The Logon password hash is pinned against an independently computed value
// rather than against this implementation's own output -- see
// kExpectedPassword. That is what caught the real bugs on the Kraken side.
// See exchanges/deribit.md and experiments/deribit_fix_probe.py.
#include "feed_handler/deribit/deribit_fix_session.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using feed_handler::deribit::fix_session;
using feed_handler::deribit::generate_logon_nonce;
using feed_handler::deribit::inbound_check;
using feed_handler::deribit::kLogonNonceBytes;
using feed_handler::deribit::make_logon_credentials;
using feed_handler::deribit::sequence_status;
using feed_handler::deribit::session_config;
using feed_handler::fix::build_message;
using feed_handler::fix::field;
using feed_handler::fix::kSoh;
using feed_handler::fix::parse_message;
namespace tag = feed_handler::fix::tag;

/// A throwaway secret that has never been a real Deribit credential.
constexpr std::string_view kTestSecret = "deribit-test-secret-do-not-use-0123456789";
constexpr std::string_view kTestClientId = "test-client";
constexpr std::uint64_t kTestTimestampMs = 1'700'000'000'000ULL;

// Known answers computed independently in Python, exactly the way
// experiments/deribit_fix_probe.py computes them:
//
//   nonce    = bytes(range(32))
//   raw_data = "1700000000000." + base64.b64encode(nonce).decode()
//   password = base64.b64encode(
//                  hashlib.sha256((raw_data + SECRET).encode()).digest()).decode()
constexpr std::string_view kExpectedRawData =
    "1700000000000.AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
constexpr std::string_view kExpectedPassword = "XXNefTCMSSiV8zBhdW1SI6lzG2CY+HcQswcBI3ywNPw=";

/// 2026-09-16T12:34:56.789Z, pinned so whole messages can be compared byte for
/// byte rather than field by field.
constexpr std::uint64_t kPinnedClockNs = 1'789'562'096'789'000'000ULL;

std::uint64_t pinned_clock() {
    return kPinnedClockNs;
}

/// The expected wire bytes of each message, written with '|' for SOH and with
/// the BodyLength/CheckSum values computed by the same independent Python
/// reference, not by this implementation. kExpectedLogon was additionally
/// checked byte for byte against simplefix's own encode() -- the library the
/// probe Deribit's testnet accepted was written with -- so the envelope
/// arithmetic is pinned against a real FIX implementation and not only against
/// a rederivation of the spec.
constexpr std::string_view kExpectedLogon =
    "8=FIX.4.4|9=206|35=A|49=test-client|56=DERIBITSERVER|34=1|52=20260916-12:34:56.789|"
    "98=0|108=30|553=test-client|"
    "96=1700000000000.AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=|"
    "554=XXNefTCMSSiV8zBhdW1SI6lzG2CY+HcQswcBI3ywNPw=|10=152|";

constexpr std::string_view kExpectedMarketDataRequest =
    "8=FIX.4.4|9=129|35=V|49=test-client|56=DERIBITSERVER|34=2|52=20260916-12:34:56.789|"
    "262=md-1|263=1|264=0|267=2|269=0|269=1|146=1|55=BTC-PERPETUAL|10=063|";

constexpr std::string_view kExpectedHeartbeat =
    "8=FIX.4.4|9=67|35=0|49=test-client|56=DERIBITSERVER|34=7|52=20260916-12:34:56.789|10=060|";

std::string wire(std::string_view readable) {
    std::string bytes(readable);
    std::ranges::replace(bytes, '|', kSoh);
    return bytes;
}

std::array<std::byte, kLogonNonceBytes> fixed_nonce() {
    std::array<std::byte, kLogonNonceBytes> nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::byte>(index);
    }
    return nonce;
}

session_config test_config() {
    return session_config{
        .client_id = std::string(kTestClientId),
        .client_secret = std::string(kTestSecret),
        .clock_ns = &pinned_clock,
    };
}

/// Builds an inbound message the way the exchange would, so the sequence
/// tracking is exercised through the same parse path the client will use.
std::string inbound(std::uint64_t seq_num, bool poss_dup = false) {
    std::vector<field> fields = {
        {.tag = tag::msg_type, .value = "0"},
        {.tag = tag::sender_comp_id, .value = "DERIBITSERVER"},
        {.tag = tag::target_comp_id, .value = std::string(kTestClientId)},
        {.tag = tag::msg_seq_num, .value = std::to_string(seq_num)},
        {.tag = tag::sending_time, .value = "20260916-12:34:56.789"},
    };
    if (poss_dup) {
        fields.push_back({.tag = tag::poss_dup_flag, .value = "Y"});
    }
    return build_message(fields);
}

inbound_check check_inbound(fix_session& session, std::uint64_t seq_num, bool poss_dup = false) {
    const std::string raw = inbound(seq_num, poss_dup);
    const auto parsed = parse_message(raw);
    EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string{} : parsed.error());
    if (!parsed) {
        return {};
    }
    return session.on_inbound(*parsed);
}

}  // namespace

// Tests live at namespace scope for the cppcheck reason documented in
// CLAUDE.md's Tests section.

TEST(DeribitLogonCredentials, MatchAnIndependentReferenceImplementation) {
    const auto nonce = fixed_nonce();
    const auto credentials = make_logon_credentials(kTestTimestampMs, nonce, kTestSecret);
    EXPECT_EQ(credentials.raw_data, kExpectedRawData);
    EXPECT_EQ(credentials.password, kExpectedPassword);
}

TEST(DeribitLogonCredentials, ChangeWhenAnyInputChanges) {
    const auto nonce = fixed_nonce();
    const auto base = make_logon_credentials(kTestTimestampMs, nonce, kTestSecret);

    auto other_nonce = nonce;
    other_nonce[0] = std::byte{0xFF};

    const auto by_timestamp = make_logon_credentials(kTestTimestampMs + 1, nonce, kTestSecret);
    const auto by_nonce = make_logon_credentials(kTestTimestampMs, other_nonce, kTestSecret);
    const auto by_secret =
        make_logon_credentials(kTestTimestampMs, nonce, "another-secret-do-not-use");

    EXPECT_NE(base.password, by_timestamp.password);
    EXPECT_NE(base.password, by_nonce.password);
    // Only the secret differs here, so RawData is identical and the password
    // must not be -- this is the assertion that proves the secret is actually
    // in the hash.
    EXPECT_EQ(base.raw_data, by_secret.raw_data);
    EXPECT_NE(base.password, by_secret.password);
}

TEST(DeribitLogonCredentials, ProduceABase64EncodedSha256Digest) {
    const auto credentials = make_logon_credentials(kTestTimestampMs, fixed_nonce(), kTestSecret);
    // base64 of a 32-byte digest is always 44 characters with one '=' of
    // padding.
    EXPECT_EQ(credentials.password.size(), 44U);
    EXPECT_EQ(credentials.password.back(), '=');
}

TEST(DeribitLogon, ProducesTheExpectedWireMessage) {
    fix_session session(test_config());
    EXPECT_EQ(session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce()),
              wire(kExpectedLogon));
}

TEST(DeribitLogon, CarriesEveryFieldDeribitRequires) {
    fix_session session(test_config());
    const std::string message = session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce());

    const auto parsed = parse_message(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->msg_type(), "A");
    EXPECT_EQ(parsed->get(tag::sender_comp_id), kTestClientId);
    EXPECT_EQ(parsed->get(tag::target_comp_id), "DERIBITSERVER");
    EXPECT_EQ(parsed->get(tag::msg_seq_num), "1");
    EXPECT_EQ(parsed->get(tag::encrypt_method), "0");
    EXPECT_EQ(parsed->get(tag::heart_bt_int), "30");
    EXPECT_EQ(parsed->get(tag::username), kTestClientId);
    EXPECT_EQ(parsed->get(tag::raw_data), kExpectedRawData);
    EXPECT_EQ(parsed->get(tag::password), kExpectedPassword);
}

TEST(DeribitLogon, NeverPutsTheClientSecretOnTheWire) {
    fix_session session(test_config());
    const std::string message = session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce());
    EXPECT_EQ(message.find(kTestSecret), std::string::npos);
}

TEST(DeribitLogon, GeneratesItsOwnNonceWhenNotGivenOne) {
    fix_session session(test_config());
    const auto first = session.build_logon();
    ASSERT_TRUE(first.has_value()) << first.error();

    fix_session other(test_config());
    const auto second = other.build_logon();
    ASSERT_TRUE(second.has_value()) << second.error();

    // Same pinned clock, same credentials: only the nonce can make these
    // differ, and it must.
    EXPECT_NE(*first, *second);
    EXPECT_TRUE(parse_message(*first).has_value());
}

TEST(DeribitNonce, IsThirtyTwoUnpredictableBytes) {
    std::set<std::string> seen;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto nonce = generate_logon_nonce();
        ASSERT_TRUE(nonce.has_value()) << nonce.error();
        EXPECT_EQ(nonce->size(), kLogonNonceBytes);
        seen.insert(std::string(std::bit_cast<const char*>(nonce->data()), nonce->size()));
    }
    EXPECT_EQ(seen.size(), 32U);
}

TEST(DeribitMarketDataRequest, ProducesTheExpectedWireMessage) {
    fix_session session(test_config());
    ASSERT_FALSE(session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce()).empty());
    EXPECT_EQ(session.build_market_data_request("md-1", "BTC-PERPETUAL"),
              wire(kExpectedMarketDataRequest));
}

TEST(DeribitMarketDataRequest, AsksForBothSidesOfTheBook) {
    fix_session session(test_config());
    const auto parsed = session.build_market_data_request("md-1", "BTC-PERPETUAL");
    const auto message = parse_message(parsed);
    ASSERT_TRUE(message.has_value()) << message.error();

    EXPECT_EQ(message->msg_type(), "V");
    EXPECT_EQ(message->get(tag::subscription_request_type), "1");  // snapshot + updates
    EXPECT_EQ(message->get(tag::market_depth), "0");               // full book
    EXPECT_EQ(message->get(tag::no_md_entry_types), "2");
    EXPECT_EQ(message->count(tag::md_entry_type), 2U);
    EXPECT_EQ(message->get(tag::no_related_sym), "1");
    EXPECT_EQ(message->get(tag::symbol), "BTC-PERPETUAL");

    // The group members must follow their count field in order -- FIX
    // repeating groups are positional, so a reordered body is a different
    // message even though get() cannot tell.
    const auto fields = message->body_fields();
    const auto count_at = std::ranges::find_if(
        fields, [](const auto& one) { return one.tag == tag::no_md_entry_types; });
    ASSERT_NE(count_at, fields.end());
    EXPECT_EQ(std::next(count_at)->tag, tag::md_entry_type);
    EXPECT_EQ(std::next(count_at)->value, "0");  // Bid
    EXPECT_EQ(std::next(count_at, 2)->tag, tag::md_entry_type);
    EXPECT_EQ(std::next(count_at, 2)->value, "1");  // Offer
}

TEST(DeribitSession, ConsumesOneOutboundSequenceNumberPerMessageInBuildOrder) {
    fix_session session(test_config());
    EXPECT_EQ(session.next_outbound_seq_num(), 1U);

    ASSERT_FALSE(session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce()).empty());
    EXPECT_EQ(session.next_outbound_seq_num(), 2U);
    ASSERT_FALSE(session.build_market_data_request("md-1", "BTC-PERPETUAL").empty());
    EXPECT_EQ(session.next_outbound_seq_num(), 3U);
    ASSERT_FALSE(session.build_heartbeat().empty());
    EXPECT_EQ(session.next_outbound_seq_num(), 4U);

    const auto parsed = parse_message(session.build_logout("done"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->get(tag::msg_seq_num), "4");
    EXPECT_EQ(parsed->get(tag::text), "done");
}

TEST(DeribitHeartbeat, ProducesTheExpectedWireMessage) {
    fix_session session(test_config());
    for (int iteration = 0; iteration < 6; ++iteration) {
        ASSERT_FALSE(session.build_heartbeat().empty());
    }
    EXPECT_EQ(session.build_heartbeat(), wire(kExpectedHeartbeat));
}

TEST(DeribitHeartbeat, EchoesTheTestRequestId) {
    // Deribit's HeartBtInt=30 means an unanswered TestRequest ends the
    // session, and the answer is only accepted if it echoes TestReqID(112).
    fix_session session(test_config());
    const auto parsed = parse_message(session.build_heartbeat_response("TEST-42"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->msg_type(), "0");
    EXPECT_EQ(parsed->get(tag::test_req_id), "TEST-42");

    // A heartbeat we originate carries no TestReqID at all.
    const auto unsolicited = parse_message(session.build_heartbeat());
    ASSERT_TRUE(unsolicited.has_value()) << unsolicited.error();
    EXPECT_FALSE(unsolicited->get(tag::test_req_id).has_value());
}

TEST(DeribitTestRequest, CarriesTheIdItExpectsBack) {
    fix_session session(test_config());
    const auto parsed = parse_message(session.build_test_request("PING-1"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->msg_type(), "1");
    EXPECT_EQ(parsed->get(tag::test_req_id), "PING-1");
}

TEST(DeribitSequence, AcceptsAContiguousInboundStream) {
    fix_session session(test_config());
    for (std::uint64_t expected = 1; expected <= 10; ++expected) {
        const auto check = check_inbound(session, expected);
        EXPECT_EQ(check.status, sequence_status::in_sequence);
        EXPECT_FALSE(check.session_broken());
        EXPECT_EQ(session.expected_inbound_seq_num(), expected + 1);
    }
    EXPECT_EQ(session.gaps_detected(), 0U);
}

TEST(DeribitSequence, DetectsAGapAndReportsHowManyWereMissed) {
    fix_session session(test_config());
    ASSERT_EQ(check_inbound(session, 1).status, sequence_status::in_sequence);
    ASSERT_EQ(check_inbound(session, 2).status, sequence_status::in_sequence);

    const auto check = check_inbound(session, 7);
    EXPECT_EQ(check.status, sequence_status::gap);
    EXPECT_TRUE(check.session_broken());
    EXPECT_EQ(check.expected, 3U);
    EXPECT_EQ(check.received, 7U);
    EXPECT_EQ(check.missing, 4U);  // 3, 4, 5, 6
    EXPECT_EQ(session.gaps_detected(), 1U);
    EXPECT_NE(check.describe().find("gap"), std::string::npos);

    // v1 detects and reports; it does not request a resend. The expectation
    // moves past the gap so the same loss is not re-reported on every
    // subsequent message while the reconnect is still in flight.
    EXPECT_EQ(session.expected_inbound_seq_num(), 8U);
    EXPECT_EQ(check_inbound(session, 8).status, sequence_status::in_sequence);
    EXPECT_EQ(session.gaps_detected(), 1U);
}

TEST(DeribitSequence, TreatsAGoingBackwardsNumberAsADuplicate) {
    fix_session session(test_config());
    ASSERT_EQ(check_inbound(session, 1).status, sequence_status::in_sequence);
    ASSERT_EQ(check_inbound(session, 2).status, sequence_status::in_sequence);

    const auto check = check_inbound(session, 2);
    EXPECT_EQ(check.status, sequence_status::duplicate);
    EXPECT_TRUE(check.session_broken());
    EXPECT_EQ(check.missing, 0U);
    // A duplicate must not rewind the expectation.
    EXPECT_EQ(session.expected_inbound_seq_num(), 3U);
}

TEST(DeribitSequence, ExemptsPossDupFlagFromGapDetection) {
    // An administratively resent message legitimately carries an already-seen
    // MsgSeqNum. Counting it as a gap or a duplicate would tear down a healthy
    // session.
    fix_session session(test_config());
    ASSERT_EQ(check_inbound(session, 1).status, sequence_status::in_sequence);
    ASSERT_EQ(check_inbound(session, 2).status, sequence_status::in_sequence);

    const auto check = check_inbound(session, 2, /*poss_dup=*/true);
    EXPECT_EQ(check.status, sequence_status::possible_duplicate);
    EXPECT_FALSE(check.session_broken());
    EXPECT_EQ(session.expected_inbound_seq_num(), 3U);
    EXPECT_EQ(session.gaps_detected(), 0U);
}

TEST(DeribitSequence, ReportsAMessageWithNoUsableMsgSeqNum) {
    // Structurally valid FIX that still cannot be sequence-checked: a missing
    // MsgSeqNum, and a present but non-numeric one. Both mean the session's
    // position is unknown, which is as untrustworthy as a gap.
    const std::vector<field> without_seq_num = {
        {.tag = tag::msg_type, .value = "0"},
        {.tag = tag::sender_comp_id, .value = "DERIBITSERVER"},
    };
    const std::vector<field> with_garbage_seq_num = {
        {.tag = tag::msg_type, .value = "0"},
        {.tag = tag::msg_seq_num, .value = "not-a-number"},
    };

    for (const auto& fields : {without_seq_num, with_garbage_seq_num}) {
        fix_session session(test_config());
        const auto parsed = parse_message(build_message(fields));
        ASSERT_TRUE(parsed.has_value()) << parsed.error();

        const auto check = session.on_inbound(*parsed);
        EXPECT_EQ(check.status, sequence_status::malformed);
        EXPECT_TRUE(check.session_broken());
        EXPECT_EQ(check.received, 0U);
        // The expectation must not move on a message that could not be read.
        EXPECT_EQ(session.expected_inbound_seq_num(), 1U);
    }
}

TEST(DeribitSequence, ResetsBothDirectionsForANewSession) {
    fix_session session(test_config());
    ASSERT_FALSE(session.build_logon_with_nonce(kTestTimestampMs, fixed_nonce()).empty());
    ASSERT_FALSE(session.build_heartbeat().empty());
    ASSERT_EQ(check_inbound(session, 1).status, sequence_status::in_sequence);
    ASSERT_EQ(check_inbound(session, 2).status, sequence_status::in_sequence);

    session.reset_sequence_numbers();

    // A reconnect is a brand new FIX session: Deribit accepted one starting at
    // MsgSeqNum 1 in the probe, without ResetSeqNumFlag(141).
    EXPECT_EQ(session.next_outbound_seq_num(), 1U);
    EXPECT_EQ(session.expected_inbound_seq_num(), 1U);
    // Gap counters are lifetime statistics, not per-session state.
    EXPECT_EQ(session.gaps_detected(), 0U);
}
