// Generic FIX.4.4 envelope arithmetic, parsing and stream framing.
//
// The envelope is the classic source of FIX bugs, so it is pinned two ways
// here: one message's expected bytes are derived by hand from the spec's own
// definitions in the comment below, and the same bytes were produced
// independently in Python before this file was written -- not by running this
// implementation and recording whatever it emitted.
#include "feed_handler/fix/fix_message.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using feed_handler::fix::build_message;
using feed_handler::fix::field;
using feed_handler::fix::format_checksum;
using feed_handler::fix::format_utc_timestamp;
using feed_handler::fix::framer;
using feed_handler::fix::kSoh;
using feed_handler::fix::parse_message;
using feed_handler::fix::read_group;
using feed_handler::fix::session_header;
namespace tag = feed_handler::fix::tag;

/// Test messages are written with '|' where the wire carries SOH -- the usual
/// FIX documentation convention, and a practical necessity: "\x0135=A" is one
/// greedy hex escape, not SOH followed by "35=A".
std::string wire(std::string_view readable) {
    std::string bytes(readable);
    std::ranges::replace(bytes, '|', kSoh);
    return bytes;
}

// The hand-verified message. Body fields, each "tag=value" plus one SOH:
//
//   35=0   -> 5 bytes    49=A   -> 5 bytes    56=B -> 5 bytes
//   34=1   -> 5 bytes    52=20260101-00:00:00.000 -> 3 + 21 + 1 = 25 bytes
//
// BodyLength(9) counts from immediately after its own trailing SOH to
// immediately before "10=", i.e. exactly those body bytes: 5+5+5+5+25 = 45.
// It covers neither "8=FIX.4.4<SOH>" nor "9=45<SOH>" nor "10=052<SOH>".
//
// CheckSum(10) is the sum of every byte from the leading '8' up to and
// including the SOH that ends "52=...", modulo 256. That sum is 15668;
// 15668 % 256 = 52, rendered as exactly three digits: "052". The zero padding
// is not cosmetic -- CheckSum is a fixed-width field and the framer relies on
// "10=NNN<SOH>" being exactly 7 bytes.
constexpr std::string_view kHandVerified =
    "8=FIX.4.4|9=45|35=0|49=A|56=B|34=1|52=20260101-00:00:00.000|10=052|";

session_header header_for(std::string_view msg_type, std::uint64_t seq_num) {
    return session_header{
        .msg_type = msg_type,
        .sender_comp_id = "A",
        .target_comp_id = "B",
        .msg_seq_num = seq_num,
        .sending_time = "20260101-00:00:00.000",
    };
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

// --- Market-data repeating groups -----------------------------------------
//
// The messages below reproduce the tag layout `exchanges/deribit.md` records
// from the live testnet capture, not a plausible-looking invention: above the
// entry group come 55, 231, 746 and Deribit's custom 100087/100090 (plus
// 100092/100093 on 35=W only), then 262 and 268; each entry is
// 269/270/271/272, preceded by 279 on 35=X only.

/// Tags this project only has to skip past, so they are named here rather than
/// in fix::tag. 746 is documented in exchanges/deribit.md by number alone.
constexpr int kContractMultiplier = 231;
constexpr int kTag746 = 746;
constexpr int kDeribitTag100087 = 100087;
constexpr int kDeribitTag100090 = 100090;
constexpr int kDeribitTag100092 = 100092;
constexpr int kDeribitTag100093 = 100093;

constexpr std::array kSnapshotEntryTags = {tag::md_entry_type, tag::md_entry_px, tag::md_entry_size,
                                           tag::md_entry_date};
constexpr std::array kIncrementalEntryTags = {tag::md_update_action, tag::md_entry_type,
                                              tag::md_entry_px, tag::md_entry_size,
                                              tag::md_entry_date};

/// One MD entry to render. An empty member is omitted from the message, which
/// is how both the 35=W/35=X shape difference (no 279 on a snapshot entry) and
/// a merely-absent optional member are expressed.
struct md_entry {
    std::string_view update_action;  // 279, 35=X only
    std::string_view entry_type;     // 269
    std::string_view price;          // 270
    std::string_view size;           // 271
    std::string_view date;           // 272
};

void append_if_present(std::vector<field>& fields, int tag_number, std::string_view value) {
    if (!value.empty()) {
        fields.push_back({.tag = tag_number, .value = std::string(value)});
    }
}

/// A `35=W` or `35=X` for BTC-PERPETUAL. `declared_count` overrides the value
/// put in NoMDEntries(268), which is how a message that lies about its own
/// count is built.
std::string md_message(std::string_view message_type, std::span<const md_entry> entries,
                       std::optional<int> declared_count = std::nullopt,
                       std::span<const field> trailing = {}) {
    const bool is_snapshot = message_type == "W";
    std::vector<field> body = {
        {.tag = tag::symbol, .value = "BTC-PERPETUAL"},
        {.tag = kContractMultiplier, .value = "10"},
        {.tag = kTag746, .value = "0"},
        {.tag = kDeribitTag100087, .value = "1"},
        {.tag = kDeribitTag100090, .value = "2"},
    };
    if (is_snapshot) {
        body.push_back({.tag = kDeribitTag100092, .value = "3"});
        body.push_back({.tag = kDeribitTag100093, .value = "4"});
    }
    body.push_back({.tag = tag::md_req_id, .value = "req-1"});
    body.push_back(
        {.tag = tag::no_md_entries,
         .value = std::to_string(declared_count.value_or(static_cast<int>(entries.size())))});

    for (const auto& entry : entries) {
        append_if_present(body, tag::md_update_action, entry.update_action);
        append_if_present(body, tag::md_entry_type, entry.entry_type);
        append_if_present(body, tag::md_entry_px, entry.price);
        append_if_present(body, tag::md_entry_size, entry.size);
        append_if_present(body, tag::md_entry_date, entry.date);
    }
    body.insert(body.end(), trailing.begin(), trailing.end());
    return build_message(header_for(message_type, 2), body);
}

}  // namespace

// Tests live at namespace scope, not inside the anonymous namespace above: the
// pre-commit cppcheck hook reports a syntaxError on a TEST macro that follows
// another definition inside an anonymous namespace (see CLAUDE.md).

TEST(FixBuilder, ProducesTheHandVerifiedEnvelope) {
    EXPECT_EQ(build_message(header_for("0", 1), {}), wire(kHandVerified));
}

TEST(FixBuilder, BodyLengthIsIndependentOfItsOwnDigitCount) {
    // A body long enough to push BodyLength from two digits to three must not
    // change the count: growing "9=99" to "9=100" adds a byte to the message
    // but none to the body.
    std::vector<field> body;
    body.push_back({.tag = tag::text, .value = std::string(60, 'x')});
    const std::string message = build_message(header_for("0", 1), body);

    const auto parsed = parse_message(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    // 45 body bytes from the header fields + "58=" + 60 x's + SOH.
    EXPECT_EQ(parsed->get(tag::body_length), "109");
    EXPECT_EQ(parsed->get(tag::text), std::string(60, 'x'));
}

TEST(FixChecksum, WrapsAtTwoFiftySixAndAlwaysRendersThreeDigits) {
    EXPECT_EQ(format_checksum(""), "000");
    EXPECT_EQ(format_checksum("A"), "065");                     // 'A' == 65
    EXPECT_EQ(format_checksum(std::string(3, '\xff')), "253");  // 765 % 256
    EXPECT_EQ(format_checksum(std::string(256, '\x01')), "000");
}

TEST(FixTimestamp, FormatsUtcWithMillisecondPrecision) {
    // 1700000000.123 epoch seconds is 2023-11-14T22:13:20.123Z.
    EXPECT_EQ(format_utc_timestamp(1'700'000'000'123'000'000ULL), "20231114-22:13:20.123");
}

TEST(FixParser, RoundTripsEveryFieldInOrder) {
    const std::array<field, 4> body = {
        field{.tag = tag::md_req_id, .value = "req-1"},
        field{.tag = tag::no_md_entry_types, .value = "2"},
        field{.tag = tag::md_entry_type, .value = "0"},
        field{.tag = tag::md_entry_type, .value = "1"},
    };
    const std::string message = build_message(header_for("V", 9), body);

    const auto parsed = parse_message(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->msg_type(), "V");

    const std::vector<std::pair<int, std::string_view>> expected = {
        {tag::msg_type, "V"},
        {tag::sender_comp_id, "A"},
        {tag::target_comp_id, "B"},
        {tag::msg_seq_num, "9"},
        {tag::sending_time, "20260101-00:00:00.000"},
        {tag::md_req_id, "req-1"},
        {tag::no_md_entry_types, "2"},
        {tag::md_entry_type, "0"},
        {tag::md_entry_type, "1"},
    };
    ASSERT_EQ(parsed->body_fields().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(parsed->body_fields()[index].tag, expected[index].first) << "at " << index;
        EXPECT_EQ(parsed->body_fields()[index].value, expected[index].second) << "at " << index;
    }

    // fields() additionally carries the envelope, body_fields() does not.
    EXPECT_EQ(parsed->size(), expected.size() + 3);
    EXPECT_EQ(parsed->fields().front().tag, tag::begin_string);
    EXPECT_EQ(parsed->fields().back().tag, tag::check_sum);
}

TEST(FixParser, GetReturnsTheFirstOccurrenceOfARepeatedTag) {
    // Unchanged by the group reader and deliberately so: get() answers about
    // the message, so for a tag that repeats inside a group its answer is
    // arbitrary. A group member is reached through read_group(), never here.
    const std::array<field, 3> body = {
        field{.tag = tag::no_md_entry_types, .value = "2"},
        field{.tag = tag::md_entry_type, .value = "0"},
        field{.tag = tag::md_entry_type, .value = "1"},
    };
    const auto parsed = parse_message(build_message(header_for("V", 1), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->count(tag::md_entry_type), 2U);
    EXPECT_EQ(parsed->get(tag::md_entry_type), "0");
}

TEST(FixParser, SplitsOnlyTheTagOnEqualsSoBase64ValuesSurvive) {
    // RawData is base64 and routinely ends in '='. Splitting a field on every
    // '=' would truncate the Logon password and the session would be rejected
    // for reasons that look nothing like a parser bug.
    constexpr std::string_view kRawData = "1700000000000.AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBka=";
    const std::array<field, 1> body = {
        field{.tag = tag::raw_data, .value = std::string(kRawData)},
    };
    const auto parsed = parse_message(build_message(header_for("A", 1), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->get(tag::raw_data), kRawData);
}

TEST(FixParser, ParsesIntegerFieldsAndRejectsNonIntegers) {
    const std::array<field, 1> body = {field{.tag = tag::text, .value = "12abc"}};
    const auto parsed = parse_message(build_message(header_for("0", 4242), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->get_int(tag::msg_seq_num), 4242);
    EXPECT_FALSE(parsed->get_int(tag::text).has_value());
    EXPECT_FALSE(parsed->get_int(tag::symbol).has_value());
}

TEST(FixParser, RejectsACorruptedCheckSum) {
    std::string message = wire(kHandVerified);
    message[message.size() - 2] = '3';  // "10=052" -> "10=053"
    const auto parsed = parse_message(message);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("CheckSum"), std::string::npos) << parsed.error();
}

TEST(FixParser, RejectsAWrongBodyLength) {
    // Same width, so nothing else about the message moves: only the claim is
    // wrong.
    std::string message = wire(kHandVerified);
    message.replace(message.find("9=45"), 4, "9=44");
    const auto parsed = parse_message(message);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("BodyLength"), std::string::npos) << parsed.error();
}

TEST(FixParser, RejectsMalformedEnvelopes) {
    EXPECT_FALSE(parse_message("").has_value());
    EXPECT_FALSE(parse_message(wire("35=0|49=A|10=000|")).has_value());
    EXPECT_FALSE(parse_message(wire("8=FIX.4.2|9=45|35=0|10=052|")).has_value());
    EXPECT_FALSE(parse_message(wire("8=FIX.4.4|35=0|49=A|10=052|")).has_value());
    // MsgType must lead the body.
    EXPECT_FALSE(parse_message(wire("8=FIX.4.4|9=10|49=A|35=0|10=000|")).has_value());
    // No CheckSum at all.
    EXPECT_FALSE(parse_message(wire("8=FIX.4.4|9=5|35=0|")).has_value());
    // Trailing bytes after CheckSum.
    EXPECT_FALSE(parse_message(wire(kHandVerified) + "junk").has_value());
}

TEST(FixFramer, SplitsSeveralMessagesOutOfOneChunk) {
    framer stream;
    stream.append(wire(kHandVerified) + wire(kHandVerified));

    ASSERT_TRUE(stream.next_message().has_value());
    ASSERT_TRUE(stream.next_message().has_value());
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_TRUE(stream.good());
    EXPECT_EQ(stream.messages_framed(), 2U);
    EXPECT_EQ(stream.buffered_bytes(), 0U);
}

TEST(FixFramer, ReassemblesAMessageDeliveredOneByteAtATime) {
    // The case a real socket produces and a "split on SOH" framer gets wrong.
    const std::string message = wire(kHandVerified);
    framer stream;
    for (std::size_t index = 0; index + 1 < message.size(); ++index) {
        stream.append(message.substr(index, 1));
        EXPECT_FALSE(stream.next_message().has_value()) << "completed early at byte " << index;
        EXPECT_TRUE(stream.good());
    }
    stream.append(message.substr(message.size() - 1));

    const auto framed = stream.next_message();
    ASSERT_TRUE(framed.has_value());
    EXPECT_EQ(*framed, message);
}

TEST(FixFramer, KeepsAPartialTrailingMessageBufferedForNextTime) {
    const std::string message = wire(kHandVerified);
    framer stream;
    stream.append(message + message.substr(0, 20));

    ASSERT_TRUE(stream.next_message().has_value());
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_EQ(stream.buffered_bytes(), 20U);

    stream.append(message.substr(20));
    const auto second = stream.next_message();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, message);
}

TEST(FixFramer, DoesNotSplitOnATenEqualsInsideAFieldValue) {
    // The whole reason the framer trusts BodyLength instead of scanning for
    // "10=": that byte sequence occurs naturally inside prices and ids.
    const std::array<field, 1> body = {field{.tag = tag::text, .value = "x10=y"}};
    const std::string message = build_message(header_for("0", 1), body);
    framer stream;
    stream.append(message);

    const auto framed = stream.next_message();
    ASSERT_TRUE(framed.has_value());
    EXPECT_EQ(*framed, message);
    EXPECT_TRUE(parse_message(*framed).has_value());
}

TEST(FixFramer, AcceptsRawBytesAsWellAsText) {
    const std::string message = wire(kHandVerified);
    framer stream;
    stream.append(bytes_of(message));
    EXPECT_TRUE(stream.next_message().has_value());
}

TEST(FixFramer, FailsStickilyOnAStreamThatIsNotFix) {
    framer stream;
    stream.append("GET / HTTP/1.1\r\n");
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_FALSE(stream.good());
    EXPECT_NE(stream.error().find("8=FIX.4.4"), std::string::npos) << stream.error();

    // Sticky: feeding it valid data afterwards must not silently resume, since
    // everything after a framing loss is unaligned.
    stream.append(wire(kHandVerified));
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_FALSE(stream.good());
}

TEST(FixFramer, WaitsRatherThanFailingOnAPartialBeginString) {
    framer stream;
    stream.append("8=FIX");
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_TRUE(stream.good());
}

TEST(FixFramer, RefusesAnAbsurdBodyLength) {
    framer stream;
    stream.append(wire("8=FIX.4.4|9=99999999|"));
    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_FALSE(stream.good());
    EXPECT_NE(stream.error().find("maximum"), std::string::npos) << stream.error();
}

TEST(FixFramer, RefusesABodyLengthThatDoesNotLandOnACheckSumField) {
    std::string message = wire(kHandVerified);
    message.replace(message.find("9=45"), 4, "9=40");
    framer stream;
    stream.append(message);

    EXPECT_FALSE(stream.next_message().has_value());
    EXPECT_FALSE(stream.good());
    EXPECT_NE(stream.error().find("CheckSum"), std::string::npos) << stream.error();
}

TEST(FixFramer, LeavesCheckSumValidationToTheParser) {
    // Framing is a transport concern; message validity is not. A message whose
    // bytes are correctly delimited but whose CheckSum is wrong is framed out
    // successfully and rejected on parse -- that split is deliberate, so the
    // client can decide what a bad CheckSum means for the session.
    std::string message = wire(kHandVerified);
    message[message.size() - 2] = '3';
    framer stream;
    stream.append(message);

    const auto framed = stream.next_message();
    ASSERT_TRUE(framed.has_value());
    EXPECT_TRUE(stream.good());
    EXPECT_FALSE(parse_message(*framed).has_value());
}

TEST(FixGroup, ReadsEveryEntryOfASnapshotInOrder) {
    const std::array<md_entry, 3> entries = {
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "64000.5",
                 .size = "10",
                 .date = "20260916"},
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "63999.0",
                 .size = "250",
                 .date = "20260916"},
        md_entry{.update_action = "",
                 .entry_type = "1",
                 .price = "64001.0",
                 .size = "7",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("W", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), entries.size());
    EXPECT_EQ(group->declared_count, entries.size());
    EXPECT_FALSE(group->truncated());

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = (*group)[index];
        EXPECT_EQ(entry.get(tag::md_entry_type), entries[index].entry_type) << "at " << index;
        EXPECT_EQ(entry.get(tag::md_entry_px), entries[index].price) << "at " << index;
        EXPECT_EQ(entry.get(tag::md_entry_size), entries[index].size) << "at " << index;
        EXPECT_EQ(entry.get(tag::md_entry_date), entries[index].date) << "at " << index;
        // A snapshot entry has no MDUpdateAction: absent, not an error.
        EXPECT_FALSE(entry.get(tag::md_update_action).has_value()) << "at " << index;
        EXPECT_EQ(entry.size(), kSnapshotEntryTags.size());
    }
    EXPECT_EQ((*group)[1].get_int(tag::md_entry_size), 250);
}

TEST(FixGroup, ReadsIncrementalEntriesWithTheirUpdateActions) {
    // 0 = New, 1 = Change, 2 = Delete -- all three observed live
    // (exchanges/deribit.md).
    const std::array<md_entry, 3> entries = {
        md_entry{.update_action = "0",
                 .entry_type = "0",
                 .price = "64000.5",
                 .size = "10",
                 .date = "20260916"},
        md_entry{.update_action = "1",
                 .entry_type = "1",
                 .price = "64001.0",
                 .size = "3",
                 .date = "20260916"},
        md_entry{.update_action = "2",
                 .entry_type = "0",
                 .price = "63998.0",
                 .size = "0",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 3U);

    std::vector<std::int64_t> actions;
    for (const auto& entry : *group) {
        const auto action = entry.get_int(tag::md_update_action);
        ASSERT_TRUE(action.has_value());
        actions.push_back(*action);
        // 279 leads an incremental entry, so it is this group's delimiter.
        EXPECT_EQ(entry.fields().front().tag, tag::md_update_action);
        EXPECT_EQ(entry.size(), kIncrementalEntryTags.size());
    }
    EXPECT_EQ(actions, (std::vector<std::int64_t>{0, 1, 2}));
    EXPECT_EQ((*group)[1].get(tag::md_entry_px), "64001.0");
    EXPECT_EQ((*group)[2].get(tag::md_entry_size), "0");
}

TEST(FixGroup, FindsBoundariesByTheDelimiterTagNotByAFixedStride) {
    // The entry in the middle omits MDEntryDate(272). A reader that assumed
    // four fields per entry would shift every later entry by one field and
    // report the wrong prices -- silently, and only on some messages.
    const std::array<md_entry, 3> entries = {
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
        md_entry{.update_action = "", .entry_type = "0", .price = "2.0", .size = "2", .date = ""},
        md_entry{.update_action = "",
                 .entry_type = "1",
                 .price = "3.0",
                 .size = "3",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("W", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 3U);
    EXPECT_EQ((*group)[0].get(tag::md_entry_px), "1.0");
    EXPECT_EQ((*group)[1].get(tag::md_entry_px), "2.0");
    EXPECT_FALSE((*group)[1].get(tag::md_entry_date).has_value());
    EXPECT_EQ((*group)[1].size(), 3U);
    EXPECT_EQ((*group)[2].get(tag::md_entry_px), "3.0");
    EXPECT_EQ((*group)[2].get(tag::md_entry_date), "20260916");
}

TEST(FixGroup, ReadsASingleEntryGroup) {
    // The commonest incremental refresh on the wire carries exactly one entry.
    const std::array<md_entry, 1> entries = {
        md_entry{.update_action = "1",
                 .entry_type = "0",
                 .price = "64000.5",
                 .size = "12",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 1U);
    EXPECT_FALSE(group->truncated());
    EXPECT_EQ(group->entries.front().get(tag::md_entry_size), "12");
}

TEST(FixGroup, ReadsAZeroEntryGroupAsEmptyRatherThanAnError) {
    const auto parsed = parse_message(md_message("X", std::span<const md_entry>{}));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->get(tag::no_md_entries), "0");

    const auto group = read_group(*parsed, tag::no_md_entries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_TRUE(group->empty());
    EXPECT_EQ(group->declared_count, 0U);
    EXPECT_FALSE(group->truncated());
}

TEST(FixGroup, StopsAtTheFirstTagThatIsNotAGroupMember) {
    const std::array<md_entry, 2> entries = {
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
        md_entry{.update_action = "",
                 .entry_type = "1",
                 .price = "2.0",
                 .size = "2",
                 .date = "20260916"},
    };
    const std::array<field, 1> trailing = {field{.tag = tag::text, .value = "after the group"}};
    const auto parsed = parse_message(md_message("W", entries, std::nullopt, trailing));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_EQ(group->entries.back().size(), kSnapshotEntryTags.size());
    EXPECT_EQ(parsed->get(tag::text), "after the group");
}

TEST(FixGroup, SalvagesTheEntriesAMessageActuallyCarriesWhenNumInGroupLies) {
    // NoMDEntries claims five, two are present: return the two and say the
    // message was short, rather than reading past the end of the field list or
    // throwing away data the caller could still inspect.
    const std::array<md_entry, 2> entries = {
        md_entry{.update_action = "0",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
        md_entry{.update_action = "2",
                 .entry_type = "1",
                 .price = "2.0",
                 .size = "0",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("X", entries, 5));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_EQ(group->declared_count, 5U);
    EXPECT_TRUE(group->truncated());
    EXPECT_EQ((*group)[0].get(tag::md_entry_px), "1.0");
    EXPECT_EQ((*group)[1].get(tag::md_entry_px), "2.0");
}

TEST(FixGroup, SurvivesAnAbsurdNumInGroupWithoutAllocatingForIt) {
    // The out-of-memory shape of the same lie: nothing may be sized from the
    // declared count. A hang or a bad_alloc here is the failure.
    const std::array<md_entry, 1> entries = {
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("W", entries, 1'000'000'000));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_EQ(group->size(), 1U);
    EXPECT_TRUE(group->truncated());
}

TEST(FixGroup, TreatsNumInGroupAsAuthoritativeWhenMoreEntriesAreOnTheWire) {
    // Three entries, a count of two: the third is outside the group as far as
    // the message's own declaration is concerned, and must not be swept in.
    const std::array<md_entry, 3> entries = {
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
        md_entry{.update_action = "",
                 .entry_type = "0",
                 .price = "2.0",
                 .size = "2",
                 .date = "20260916"},
        md_entry{.update_action = "",
                 .entry_type = "1",
                 .price = "3.0",
                 .size = "3",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("W", entries, 2));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_FALSE(group->truncated());
    EXPECT_EQ((*group)[1].get(tag::md_entry_px), "2.0");
}

TEST(FixGroup, ReadsNothingWhenAskedForTheWrongEntryShape) {
    // 35=X entries open with 279, which the 35=W member set does not contain,
    // so the group is reported empty-and-short rather than mis-parsed. The
    // shape belongs to the call, not to a guess made inside the reader.
    const std::array<md_entry, 2> entries = {
        md_entry{.update_action = "0",
                 .entry_type = "0",
                 .price = "1.0",
                 .size = "1",
                 .date = "20260916"},
        md_entry{.update_action = "1",
                 .entry_type = "1",
                 .price = "2.0",
                 .size = "2",
                 .date = "20260916"},
    };
    const auto parsed = parse_message(md_message("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_TRUE(group->empty());
    EXPECT_TRUE(group->truncated());
}

TEST(FixGroup, ReadsTheOtherGroupsAMarketDataRequestCarries) {
    // NoMDEntryTypes(267) is a one-member group -- delimiter and entry are the
    // same tag -- and the reader must not need a second member to find its
    // boundaries.
    const std::array<field, 5> body = {
        field{.tag = tag::no_md_entry_types, .value = "2"},
        field{.tag = tag::md_entry_type, .value = "0"},
        field{.tag = tag::md_entry_type, .value = "1"},
        field{.tag = tag::no_related_sym, .value = "1"},
        field{.tag = tag::symbol, .value = "BTC-PERPETUAL"},
    };
    const auto parsed = parse_message(build_message(header_for("V", 3), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto types = read_group(*parsed, tag::no_md_entry_types, {tag::md_entry_type});
    ASSERT_TRUE(types.has_value()) << types.error();
    ASSERT_EQ(types->size(), 2U);
    EXPECT_EQ((*types)[0].get(tag::md_entry_type), "0");
    EXPECT_EQ((*types)[1].get(tag::md_entry_type), "1");

    const auto symbols = read_group(*parsed, tag::no_related_sym, {tag::symbol});
    ASSERT_TRUE(symbols.has_value()) << symbols.error();
    ASSERT_EQ(symbols->size(), 1U);
    EXPECT_EQ(symbols->entries.front().get(tag::symbol), "BTC-PERPETUAL");
}

TEST(FixGroup, FailsOnlyWhenThereIsNoGroupToRead) {
    const auto parsed = parse_message(wire(kHandVerified));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto missing = read_group(*parsed, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("NumInGroup"), std::string::npos) << missing.error();

    const std::array<field, 2> body = {
        field{.tag = tag::no_md_entries, .value = "two"},
        field{.tag = tag::md_entry_type, .value = "0"},
    };
    const auto bad_count = parse_message(build_message(header_for("X", 1), body));
    ASSERT_TRUE(bad_count.has_value()) << bad_count.error();
    const auto unparsable = read_group(*bad_count, tag::no_md_entries, kSnapshotEntryTags);
    ASSERT_FALSE(unparsable.has_value());
    EXPECT_NE(unparsable.error().find("non-negative"), std::string::npos) << unparsable.error();
}
