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

using feed_handler::deribit::FixSession;
using feed_handler::deribit::GenerateLogonNonce;
using feed_handler::deribit::InboundCheck;
using feed_handler::deribit::kLogonNonceBytes;
using feed_handler::deribit::MakeLogonCredentials;
using feed_handler::deribit::SequenceStatus;
using feed_handler::deribit::SessionConfig;
using feed_handler::fix::BuildMessage;
using feed_handler::fix::Field;
using feed_handler::fix::kSoh;
using feed_handler::fix::ParseMessage;
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

std::uint64_t PinnedClock() {
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

std::string Wire(std::string_view readable) {
    std::string bytes(readable);
    std::ranges::replace(bytes, '|', kSoh);
    return bytes;
}

std::array<std::byte, kLogonNonceBytes> FixedNonce() {
    std::array<std::byte, kLogonNonceBytes> nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::byte>(index);
    }
    return nonce;
}

SessionConfig TestConfig() {
    return SessionConfig{
        .client_id = std::string(kTestClientId),
        .client_secret = std::string(kTestSecret),
        .clock_ns = &PinnedClock,
    };
}

/// Builds an inbound message the way the exchange would, so the sequence
/// tracking is exercised through the same parse path the client will use.
std::string Inbound(std::uint64_t seq_num, bool poss_dup = false) {
    std::vector<Field> fields = {
        {.tag = tag::kMsgType, .value = "0"},
        {.tag = tag::kSenderCompId, .value = "DERIBITSERVER"},
        {.tag = tag::kTargetCompId, .value = std::string(kTestClientId)},
        {.tag = tag::kMsgSeqNum, .value = std::to_string(seq_num)},
        {.tag = tag::kSendingTime, .value = "20260916-12:34:56.789"},
    };
    if (poss_dup) {
        fields.push_back({.tag = tag::kPossDupFlag, .value = "Y"});
    }
    return BuildMessage(fields);
}

InboundCheck CheckInbound(FixSession& session, std::uint64_t seq_num, bool poss_dup = false) {
    const std::string raw = Inbound(seq_num, poss_dup);
    const auto parsed = ParseMessage(raw);
    EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string{} : parsed.error());
    if (!parsed) {
        return {};
    }
    return session.OnInbound(*parsed);
}

}  // namespace

// Tests live at namespace scope for the cppcheck reason documented in
// CLAUDE.md's Tests section.

TEST(DeribitLogonCredentials, MatchAnIndependentReferenceImplementation) {
    const auto nonce = FixedNonce();
    const auto credentials = MakeLogonCredentials(kTestTimestampMs, nonce, kTestSecret);
    EXPECT_EQ(credentials.raw_data, kExpectedRawData);
    EXPECT_EQ(credentials.password, kExpectedPassword);
}

TEST(DeribitLogonCredentials, ChangeWhenAnyInputChanges) {
    const auto nonce = FixedNonce();
    const auto base = MakeLogonCredentials(kTestTimestampMs, nonce, kTestSecret);

    auto other_nonce = nonce;
    other_nonce[0] = std::byte{0xFF};

    const auto by_timestamp = MakeLogonCredentials(kTestTimestampMs + 1, nonce, kTestSecret);
    const auto by_nonce = MakeLogonCredentials(kTestTimestampMs, other_nonce, kTestSecret);
    const auto by_secret =
        MakeLogonCredentials(kTestTimestampMs, nonce, "another-secret-do-not-use");

    EXPECT_NE(base.password, by_timestamp.password);
    EXPECT_NE(base.password, by_nonce.password);
    // Only the secret differs here, so RawData is identical and the password
    // must not be -- this is the assertion that proves the secret is actually
    // in the hash.
    EXPECT_EQ(base.raw_data, by_secret.raw_data);
    EXPECT_NE(base.password, by_secret.password);
}

TEST(DeribitLogonCredentials, ProduceABase64EncodedSha256Digest) {
    const auto credentials = MakeLogonCredentials(kTestTimestampMs, FixedNonce(), kTestSecret);
    // base64 of a 32-byte digest is always 44 characters with one '=' of
    // padding.
    EXPECT_EQ(credentials.password.size(), 44U);
    EXPECT_EQ(credentials.password.back(), '=');
}

TEST(DeribitLogon, ProducesTheExpectedWireMessage) {
    FixSession session(TestConfig());
    EXPECT_EQ(session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce()), Wire(kExpectedLogon));
}

TEST(DeribitLogon, CarriesEveryFieldDeribitRequires) {
    FixSession session(TestConfig());
    const std::string message = session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce());

    const auto parsed = ParseMessage(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->MsgType(), "A");
    EXPECT_EQ(parsed->Get(tag::kSenderCompId), kTestClientId);
    EXPECT_EQ(parsed->Get(tag::kTargetCompId), "DERIBITSERVER");
    EXPECT_EQ(parsed->Get(tag::kMsgSeqNum), "1");
    EXPECT_EQ(parsed->Get(tag::kEncryptMethod), "0");
    EXPECT_EQ(parsed->Get(tag::kHeartBtInt), "30");
    EXPECT_EQ(parsed->Get(tag::kUsername), kTestClientId);
    EXPECT_EQ(parsed->Get(tag::kRawData), kExpectedRawData);
    EXPECT_EQ(parsed->Get(tag::kPassword), kExpectedPassword);
}

TEST(DeribitLogon, NeverPutsTheClientSecretOnTheWire) {
    FixSession session(TestConfig());
    const std::string message = session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce());
    EXPECT_EQ(message.find(kTestSecret), std::string::npos);
}

TEST(DeribitLogon, GeneratesItsOwnNonceWhenNotGivenOne) {
    FixSession session(TestConfig());
    const auto first = session.BuildLogon();
    ASSERT_TRUE(first.has_value()) << first.error();

    FixSession other(TestConfig());
    const auto second = other.BuildLogon();
    ASSERT_TRUE(second.has_value()) << second.error();

    // Same pinned clock, same credentials: only the nonce can make these
    // differ, and it must.
    EXPECT_NE(*first, *second);
    EXPECT_TRUE(ParseMessage(*first).has_value());
}

TEST(DeribitNonce, IsThirtyTwoUnpredictableBytes) {
    std::set<std::string> seen;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto nonce = GenerateLogonNonce();
        ASSERT_TRUE(nonce.has_value()) << nonce.error();
        EXPECT_EQ(nonce->size(), kLogonNonceBytes);
        seen.insert(std::string(std::bit_cast<const char*>(nonce->data()), nonce->size()));
    }
    EXPECT_EQ(seen.size(), 32U);
}

TEST(DeribitMarketDataRequest, ProducesTheExpectedWireMessage) {
    FixSession session(TestConfig());
    ASSERT_FALSE(session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce()).empty());
    EXPECT_EQ(session.BuildMarketDataRequest("md-1", "BTC-PERPETUAL"),
              Wire(kExpectedMarketDataRequest));
}

TEST(DeribitMarketDataRequest, AsksForBothSidesOfTheBook) {
    FixSession session(TestConfig());
    const auto parsed = session.BuildMarketDataRequest("md-1", "BTC-PERPETUAL");
    const auto message = ParseMessage(parsed);
    ASSERT_TRUE(message.has_value()) << message.error();

    EXPECT_EQ(message->MsgType(), "V");
    EXPECT_EQ(message->Get(tag::kSubscriptionRequestType), "1");  // snapshot + updates
    EXPECT_EQ(message->Get(tag::kMarketDepth), "0");              // full book
    EXPECT_EQ(message->Get(tag::kNoMdEntryTypes), "2");
    EXPECT_EQ(message->Count(tag::kMdEntryType), 2U);
    EXPECT_EQ(message->Get(tag::kNoRelatedSym), "1");
    EXPECT_EQ(message->Get(tag::kSymbol), "BTC-PERPETUAL");

    // The group members must follow their count field in order -- FIX
    // repeating groups are positional, so a reordered body is a different
    // message even though get() cannot tell.
    const auto fields = message->BodyFields();
    const auto count_at = std::ranges::find_if(
        fields, [](const auto& one) { return one.tag == tag::kNoMdEntryTypes; });
    ASSERT_NE(count_at, fields.end());
    EXPECT_EQ(std::next(count_at)->tag, tag::kMdEntryType);
    EXPECT_EQ(std::next(count_at)->value, "0");  // Bid
    EXPECT_EQ(std::next(count_at, 2)->tag, tag::kMdEntryType);
    EXPECT_EQ(std::next(count_at, 2)->value, "1");  // Offer
}

TEST(DeribitSession, ConsumesOneOutboundSequenceNumberPerMessageInBuildOrder) {
    FixSession session(TestConfig());
    EXPECT_EQ(session.NextOutboundSeqNum(), 1U);

    ASSERT_FALSE(session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce()).empty());
    EXPECT_EQ(session.NextOutboundSeqNum(), 2U);
    ASSERT_FALSE(session.BuildMarketDataRequest("md-1", "BTC-PERPETUAL").empty());
    EXPECT_EQ(session.NextOutboundSeqNum(), 3U);
    ASSERT_FALSE(session.BuildHeartbeat().empty());
    EXPECT_EQ(session.NextOutboundSeqNum(), 4U);

    const std::string logout = session.BuildLogout("done");
    const auto parsed = ParseMessage(logout);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->Get(tag::kMsgSeqNum), "4");
    EXPECT_EQ(parsed->Get(tag::kText), "done");
}

TEST(DeribitHeartbeat, ProducesTheExpectedWireMessage) {
    FixSession session(TestConfig());
    for (int iteration = 0; iteration < 6; ++iteration) {
        ASSERT_FALSE(session.BuildHeartbeat().empty());
    }
    EXPECT_EQ(session.BuildHeartbeat(), Wire(kExpectedHeartbeat));
}

TEST(DeribitHeartbeat, EchoesTheTestRequestId) {
    // Deribit's HeartBtInt=30 means an unanswered TestRequest ends the
    // session, and the answer is only accepted if it echoes TestReqID(112).
    FixSession session(TestConfig());
    const std::string response = session.BuildHeartbeatResponse("TEST-42");
    const auto parsed = ParseMessage(response);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->MsgType(), "0");
    EXPECT_EQ(parsed->Get(tag::kTestReqId), "TEST-42");

    // A heartbeat we originate carries no TestReqID at all.
    const std::string heartbeat = session.BuildHeartbeat();
    const auto unsolicited = ParseMessage(heartbeat);
    ASSERT_TRUE(unsolicited.has_value()) << unsolicited.error();
    EXPECT_FALSE(unsolicited->Get(tag::kTestReqId).has_value());
}

TEST(DeribitTestRequest, CarriesTheIdItExpectsBack) {
    FixSession session(TestConfig());
    const std::string test_request = session.BuildTestRequest("PING-1");
    const auto parsed = ParseMessage(test_request);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->MsgType(), "1");
    EXPECT_EQ(parsed->Get(tag::kTestReqId), "PING-1");
}

TEST(DeribitSequence, AcceptsAContiguousInboundStream) {
    FixSession session(TestConfig());
    for (std::uint64_t expected = 1; expected <= 10; ++expected) {
        const auto check = CheckInbound(session, expected);
        EXPECT_EQ(check.status, SequenceStatus::kInSequence);
        EXPECT_FALSE(check.SessionBroken());
        EXPECT_EQ(session.ExpectedInboundSeqNum(), expected + 1);
    }
    EXPECT_EQ(session.GapsDetected(), 0U);
}

TEST(DeribitSequence, DetectsAGapAndReportsHowManyWereMissed) {
    FixSession session(TestConfig());
    ASSERT_EQ(CheckInbound(session, 1).status, SequenceStatus::kInSequence);
    ASSERT_EQ(CheckInbound(session, 2).status, SequenceStatus::kInSequence);

    const auto check = CheckInbound(session, 7);
    EXPECT_EQ(check.status, SequenceStatus::kGap);
    EXPECT_TRUE(check.SessionBroken());
    EXPECT_EQ(check.expected, 3U);
    EXPECT_EQ(check.received, 7U);
    EXPECT_EQ(check.missing, 4U);  // 3, 4, 5, 6
    EXPECT_EQ(session.GapsDetected(), 1U);
    EXPECT_NE(check.Describe().find("gap"), std::string::npos);

    // v1 detects and reports; it does not request a resend. The expectation
    // moves past the gap so the same loss is not re-reported on every
    // subsequent message while the reconnect is still in flight.
    EXPECT_EQ(session.ExpectedInboundSeqNum(), 8U);
    EXPECT_EQ(CheckInbound(session, 8).status, SequenceStatus::kInSequence);
    EXPECT_EQ(session.GapsDetected(), 1U);
}

TEST(DeribitSequence, TreatsAGoingBackwardsNumberAsADuplicate) {
    FixSession session(TestConfig());
    ASSERT_EQ(CheckInbound(session, 1).status, SequenceStatus::kInSequence);
    ASSERT_EQ(CheckInbound(session, 2).status, SequenceStatus::kInSequence);

    const auto check = CheckInbound(session, 2);
    EXPECT_EQ(check.status, SequenceStatus::kDuplicate);
    EXPECT_TRUE(check.SessionBroken());
    EXPECT_EQ(check.missing, 0U);
    // A duplicate must not rewind the expectation.
    EXPECT_EQ(session.ExpectedInboundSeqNum(), 3U);
}

TEST(DeribitSequence, ExemptsPossDupFlagFromGapDetection) {
    // An administratively resent message legitimately carries an already-seen
    // MsgSeqNum. Counting it as a gap or a duplicate would tear down a healthy
    // session.
    FixSession session(TestConfig());
    ASSERT_EQ(CheckInbound(session, 1).status, SequenceStatus::kInSequence);
    ASSERT_EQ(CheckInbound(session, 2).status, SequenceStatus::kInSequence);

    const auto check = CheckInbound(session, 2, /*poss_dup=*/true);
    EXPECT_EQ(check.status, SequenceStatus::kPossibleDuplicate);
    EXPECT_FALSE(check.SessionBroken());
    EXPECT_EQ(session.ExpectedInboundSeqNum(), 3U);
    EXPECT_EQ(session.GapsDetected(), 0U);
}

TEST(DeribitSequence, ReportsAMessageWithNoUsableMsgSeqNum) {
    // Structurally valid FIX that still cannot be sequence-checked: a missing
    // MsgSeqNum, and a present but non-numeric one. Both mean the session's
    // position is unknown, which is as untrustworthy as a gap.
    const std::vector<Field> without_seq_num = {
        {.tag = tag::kMsgType, .value = "0"},
        {.tag = tag::kSenderCompId, .value = "DERIBITSERVER"},
    };
    const std::vector<Field> with_garbage_seq_num = {
        {.tag = tag::kMsgType, .value = "0"},
        {.tag = tag::kMsgSeqNum, .value = "not-a-number"},
    };

    for (const auto& fields : {without_seq_num, with_garbage_seq_num}) {
        FixSession session(TestConfig());
        const std::string raw = BuildMessage(fields);
        const auto parsed = ParseMessage(raw);
        ASSERT_TRUE(parsed.has_value()) << parsed.error();

        const auto check = session.OnInbound(*parsed);
        EXPECT_EQ(check.status, SequenceStatus::kMalformed);
        EXPECT_TRUE(check.SessionBroken());
        EXPECT_EQ(check.received, 0U);
        // The expectation must not move on a message that could not be read.
        EXPECT_EQ(session.ExpectedInboundSeqNum(), 1U);
    }
}

TEST(DeribitSequence, ResetsBothDirectionsForANewSession) {
    FixSession session(TestConfig());
    ASSERT_FALSE(session.BuildLogonWithNonce(kTestTimestampMs, FixedNonce()).empty());
    ASSERT_FALSE(session.BuildHeartbeat().empty());
    ASSERT_EQ(CheckInbound(session, 1).status, SequenceStatus::kInSequence);
    ASSERT_EQ(CheckInbound(session, 2).status, SequenceStatus::kInSequence);

    session.ResetSequenceNumbers();

    // A reconnect is a brand new FIX session: Deribit accepted one starting at
    // MsgSeqNum 1 in the probe, without ResetSeqNumFlag(141).
    EXPECT_EQ(session.NextOutboundSeqNum(), 1U);
    EXPECT_EQ(session.ExpectedInboundSeqNum(), 1U);
    // Gap counters are lifetime statistics, not per-session state.
    EXPECT_EQ(session.GapsDetected(), 0U);
}
