// The socket-independent half of the Deribit FIX client: the "given this
// message and this sequence verdict, what happens next" decision, and the
// reconnect backoff curve.
//
// Same split as capture_session/staleness_watchdog on the Kraken side -- the
// branches worth getting right are the ones a live socket makes hardest to
// exercise (a gap, a peer-initiated Logout, a rejected subscribe on an
// otherwise healthy session), so they are pulled out into pure functions and
// tested here. Actual socket I/O is proven by the live testnet run, not by a
// mock.
#include "feed_handler/deribit/deribit_fix_client.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using feed_handler::deribit::classify_inbound;
using feed_handler::deribit::fix_session;
using feed_handler::deribit::inbound_action;
using feed_handler::deribit::inbound_check;
using feed_handler::deribit::inbound_kind;
using feed_handler::deribit::reconnect_delay_ms;
using feed_handler::deribit::sequence_status;
using feed_handler::deribit::session_config;
using feed_handler::fix::field;
using feed_handler::fix::parse_message;
namespace msg_type = feed_handler::fix::msg_type;
namespace tag = feed_handler::fix::tag;

/// A throwaway secret that has never been a real Deribit credential.
constexpr std::string_view kTestSecret = "deribit-test-secret-do-not-use-0123456789";
constexpr std::string_view kTestClientId = "test-client";

/// Renders an inbound message the way Deribit would: it is the sender, we are
/// the target.
std::string inbound(std::string_view type, std::uint64_t seq_num,
                    std::span<const field> body = {}) {
    const feed_handler::fix::session_header header{
        .msg_type = type,
        .sender_comp_id = "DERIBITSERVER",
        .target_comp_id = kTestClientId,
        .msg_seq_num = seq_num,
        .sending_time = "20260916-21:30:00.000",
    };
    return feed_handler::fix::build_message(header, body);
}

session_config test_config() {
    return session_config{
        .client_id = std::string(kTestClientId),
        .client_secret = std::string(kTestSecret),
    };
}

/// The verdict for a message that arrived exactly where it was expected, so a
/// test can isolate the message-type branch from the sequence branch.
inbound_check in_sequence(std::uint64_t seq_num) {
    return inbound_check{
        .status = sequence_status::in_sequence,
        .expected = seq_num,
        .received = seq_num,
        .missing = 0,
    };
}

}  // namespace

// Tests at namespace scope, after the anonymous namespace closes: cppcheck
// cannot parse TEST macros that follow another definition inside one.

TEST(DeribitInboundDecision, AnAcceptedLogonTriggersTheMarketDataRequest) {
    // Deribit echoes the accepted Logon back as its own 35=A before anything
    // else (experiments/deribit_fix_probe.py), which is the cue to subscribe.
    const std::array<field, 2> body = {
        field{.tag = tag::encrypt_method, .value = "0"},
        field{.tag = tag::heart_bt_int, .value = "30"},
    };
    const std::string raw = inbound(msg_type::logon, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(1));
    EXPECT_EQ(decision.kind, inbound_kind::logon_ack);
    EXPECT_EQ(decision.action, inbound_action::send_market_data_request);
}

TEST(DeribitInboundDecision, ALogonDecisionNeverCarriesCredentialFieldsInItsDetail) {
    // The echoed Logon contains RawData(96) and Password(554). `detail` is what
    // reaches a log line, so nothing derived from those fields may end up in it.
    const std::array<field, 2> body = {
        field{.tag = tag::raw_data, .value = "1700000000000.bm9uY2U="},
        field{.tag = tag::password, .value = "c2VjcmV0LWxvb2tpbmctdmFsdWU="},
    };
    const std::string raw = inbound(msg_type::logon, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    EXPECT_EQ(classify_inbound(*parsed, in_sequence(1)).detail, "");
}

TEST(DeribitInboundDecision, ATestRequestIsAnsweredWithTheEchoedTestReqId) {
    // An unanswered TestRequest ends the session (exchanges/deribit.md), and
    // the answer is only accepted if it echoes this exact id.
    const std::array<field, 1> body = {
        field{.tag = tag::test_req_id, .value = "TEST-4711"},
    };
    const std::string raw = inbound(msg_type::test_request, 7, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(7));
    EXPECT_EQ(decision.kind, inbound_kind::test_request);
    EXPECT_EQ(decision.action, inbound_action::answer_test_request);
    EXPECT_EQ(decision.detail, "TEST-4711");
}

TEST(DeribitInboundDecision, SeparatesSnapshotsFromIncrementalRefreshes) {
    const std::string snapshot = inbound(msg_type::market_data_snapshot_full_refresh, 3);
    const auto parsed_snapshot = parse_message(snapshot);
    ASSERT_TRUE(parsed_snapshot.has_value()) << parsed_snapshot.error();
    const auto snapshot_decision = classify_inbound(*parsed_snapshot, in_sequence(3));
    EXPECT_EQ(snapshot_decision.kind, inbound_kind::market_data_snapshot);
    EXPECT_EQ(snapshot_decision.action, inbound_action::none);

    const std::string incremental = inbound(msg_type::market_data_incremental_refresh, 4);
    const auto parsed_incremental = parse_message(incremental);
    ASSERT_TRUE(parsed_incremental.has_value()) << parsed_incremental.error();
    const auto incremental_decision = classify_inbound(*parsed_incremental, in_sequence(4));
    EXPECT_EQ(incremental_decision.kind, inbound_kind::market_data_incremental);
    EXPECT_EQ(incremental_decision.action, inbound_action::none);
}

TEST(DeribitInboundDecision, ARejectedMarketDataRequestKeepsTheSessionButKeepsTheReason) {
    // The failure mode this exists for: the session stays up, the heartbeats
    // keep flowing, and no market data ever arrives. Reconnecting would only
    // repeat the rejected request, so the answer is a loud log, not a retry.
    const std::array<field, 2> body = {
        field{.tag = tag::md_req_id, .value = "ct-md-1"},
        field{.tag = tag::text, .value = "unknown instrument"},
    };
    const std::string raw = inbound(msg_type::market_data_request_reject, 3, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(3));
    EXPECT_EQ(decision.kind, inbound_kind::market_data_request_reject);
    EXPECT_EQ(decision.action, inbound_action::none);
    EXPECT_EQ(decision.detail, "unknown instrument");
}

TEST(DeribitInboundDecision, APeerInitiatedLogoutEndsTheSession) {
    const std::array<field, 1> body = {
        field{.tag = tag::text, .value = "session expired"},
    };
    const std::string raw = inbound(msg_type::logout, 9, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(9));
    EXPECT_EQ(decision.kind, inbound_kind::logout);
    EXPECT_EQ(decision.action, inbound_action::reconnect);
    EXPECT_EQ(decision.detail, "session expired");
}

TEST(DeribitInboundDecision, ASequenceGapOutranksWhateverTheMessageSays) {
    // decisions/0004: no ResendRequest/SequenceReset gap fill. The messages in
    // the gap are gone, so the book this snapshot would seed cannot be trusted
    // either -- drop the session and start a fresh one.
    fix_session session(test_config());
    const std::string raw = inbound(msg_type::market_data_incremental_refresh, 5);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const inbound_check check = session.on_inbound(*parsed);
    ASSERT_EQ(check.status, sequence_status::gap);
    const auto decision = classify_inbound(*parsed, check);
    EXPECT_EQ(decision.kind, inbound_kind::market_data_incremental);
    EXPECT_EQ(decision.action, inbound_action::reconnect);
    EXPECT_NE(decision.detail.find("gap"), std::string::npos);
}

TEST(DeribitInboundDecision, AnAdministrativeResendIsNotASessionBreak) {
    // PossDupFlag(43)=Y legitimately repeats a sequence number; tearing the
    // session down over one would be self-inflicted.
    fix_session session(test_config());
    const std::array<field, 1> body = {
        field{.tag = tag::poss_dup_flag, .value = "Y"},
    };
    const std::string raw = inbound(msg_type::heartbeat, 1, body);
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const inbound_check check = session.on_inbound(*parsed);
    ASSERT_EQ(check.status, sequence_status::possible_duplicate);
    const auto decision = classify_inbound(*parsed, check);
    EXPECT_EQ(decision.kind, inbound_kind::heartbeat);
    EXPECT_EQ(decision.action, inbound_action::none);
}

TEST(DeribitInboundDecision, AnUnrecognisedMessageTypeIsJournaledAndOtherwiseIgnored) {
    // "unknown" must stay inert: the message has already been journaled by the
    // time this runs, and a type this build does not know is not a reason to
    // drop a working session.
    const std::string raw = inbound("n", 2);  // XMLnonFIX, which this build never asked for.
    const auto parsed = parse_message(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto decision = classify_inbound(*parsed, in_sequence(2));
    EXPECT_EQ(decision.kind, inbound_kind::unknown);
    EXPECT_EQ(decision.action, inbound_action::none);
}

TEST(DeribitReconnectBackoff, DoesNotDelayTheFirstConnectionAttempt) {
    EXPECT_EQ(reconnect_delay_ms(0, 1'000, 30'000), 0U);
}

TEST(DeribitReconnectBackoff, DoublesFromTheFloorAndSaturatesAtTheCap) {
    EXPECT_EQ(reconnect_delay_ms(1, 1'000, 30'000), 1'000U);
    EXPECT_EQ(reconnect_delay_ms(2, 1'000, 30'000), 2'000U);
    EXPECT_EQ(reconnect_delay_ms(3, 1'000, 30'000), 4'000U);
    EXPECT_EQ(reconnect_delay_ms(5, 1'000, 30'000), 16'000U);
    EXPECT_EQ(reconnect_delay_ms(6, 1'000, 30'000), 30'000U);
}

TEST(DeribitReconnectBackoff, StaysAtTheCapAcrossALongOutage) {
    // The shift is capped before the clamp so a day-long outage cannot shift a
    // 64-bit value past its width -- that is undefined behaviour, not a large
    // number, and it would be found by an outage rather than by a test run.
    EXPECT_EQ(reconnect_delay_ms(64, 1'000, 30'000), 30'000U);
    EXPECT_EQ(reconnect_delay_ms(100'000, 1'000, 30'000), 30'000U);
}
