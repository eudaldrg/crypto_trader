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

using feed_handler::fix::BuildMessage;
using feed_handler::fix::Field;
using feed_handler::fix::FormatChecksum;
using feed_handler::fix::FormatUtcTimestamp;
using feed_handler::fix::Framer;
using feed_handler::fix::kSoh;
using feed_handler::fix::ParsedMessage;
using feed_handler::fix::ParseMessage;
using feed_handler::fix::ReadGroup;
using feed_handler::fix::SessionHeader;
namespace tag = feed_handler::fix::tag;

/// Test messages are written with '|' where the wire carries SOH -- the usual
/// FIX documentation convention, and a practical necessity: "\x0135=A" is one
/// greedy hex escape, not SOH followed by "35=A".
std::string Wire(std::string_view readable) {
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

SessionHeader HeaderFor(std::string_view msg_type, std::uint64_t seq_num) {
    return SessionHeader{
        .msg_type = msg_type,
        .sender_comp_id = "A",
        .target_comp_id = "B",
        .msg_seq_num = seq_num,
        .sending_time = "20260101-00:00:00.000",
    };
}

std::span<const std::byte> BytesOf(std::string_view text) {
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

constexpr std::array kSnapshotEntryTags = {tag::kMdEntryType, tag::kMdEntryPx, tag::kMdEntrySize,
                                           tag::kMdEntryDate};
constexpr std::array kIncrementalEntryTags = {
    tag::kMdUpdateAction, tag::kMdEntryType, tag::kMdEntryPx, tag::kMdEntrySize, tag::kMdEntryDate};

/// One MD entry to render. An empty member is omitted from the message, which
/// is how both the 35=W/35=X shape difference (no 279 on a snapshot entry) and
/// a merely-absent optional member are expressed.
struct MdEntry {
    std::string_view update_action;  // 279, 35=X only
    std::string_view entry_type;     // 269
    std::string_view price;          // 270
    std::string_view size;           // 271
    std::string_view date;           // 272
};

void AppendIfPresent(std::vector<Field>& fields, int tag_number, std::string_view value) {
    if (!value.empty()) {
        fields.push_back({.tag = tag_number, .value = std::string(value)});
    }
}

/// A `35=W` or `35=X` for BTC-PERPETUAL. `declared_count` overrides the value
/// put in NoMDEntries(268), which is how a message that lies about its own
/// count is built.
std::string MdMessage(std::string_view message_type, std::span<const MdEntry> entries,
                      std::optional<int> declared_count = std::nullopt,
                      std::span<const Field> trailing = {}) {
    const bool is_snapshot = message_type == "W";
    std::vector<Field> body = {
        {.tag = tag::kSymbol, .value = "BTC-PERPETUAL"},
        {.tag = kContractMultiplier, .value = "10"},
        {.tag = kTag746, .value = "0"},
        {.tag = kDeribitTag100087, .value = "1"},
        {.tag = kDeribitTag100090, .value = "2"},
    };
    if (is_snapshot) {
        body.push_back({.tag = kDeribitTag100092, .value = "3"});
        body.push_back({.tag = kDeribitTag100093, .value = "4"});
    }
    body.push_back({.tag = tag::kMdReqId, .value = "req-1"});
    body.push_back(
        {.tag = tag::kNoMdEntries,
         .value = std::to_string(declared_count.value_or(static_cast<int>(entries.size())))});

    for (const auto& entry : entries) {
        AppendIfPresent(body, tag::kMdUpdateAction, entry.update_action);
        AppendIfPresent(body, tag::kMdEntryType, entry.entry_type);
        AppendIfPresent(body, tag::kMdEntryPx, entry.price);
        AppendIfPresent(body, tag::kMdEntrySize, entry.size);
        AppendIfPresent(body, tag::kMdEntryDate, entry.date);
    }
    body.insert(body.end(), trailing.begin(), trailing.end());
    return BuildMessage(HeaderFor(message_type, 2), body);
}

}  // namespace

// Tests live at namespace scope, not inside the anonymous namespace above: the
// pre-commit cppcheck hook reports a syntaxError on a TEST macro that follows
// another definition inside an anonymous namespace (see CLAUDE.md).

TEST(FixBuilder, ProducesTheHandVerifiedEnvelope) {
    EXPECT_EQ(BuildMessage(HeaderFor("0", 1), {}), Wire(kHandVerified));
}

TEST(FixBuilder, BodyLengthIsIndependentOfItsOwnDigitCount) {
    // A body long enough to push BodyLength from two digits to three must not
    // change the count: growing "9=99" to "9=100" adds a byte to the message
    // but none to the body.
    std::vector<Field> body;
    body.push_back({.tag = tag::kText, .value = std::string(60, 'x')});
    const std::string message = BuildMessage(HeaderFor("0", 1), body);

    const auto parsed = ParseMessage(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    // 45 body bytes from the header fields + "58=" + 60 x's + SOH.
    EXPECT_EQ(parsed->Get(tag::kBodyLength), "109");
    EXPECT_EQ(parsed->Get(tag::kText), std::string(60, 'x'));
}

TEST(FixChecksum, WrapsAtTwoFiftySixAndAlwaysRendersThreeDigits) {
    EXPECT_EQ(FormatChecksum(""), "000");
    EXPECT_EQ(FormatChecksum("A"), "065");                     // 'A' == 65
    EXPECT_EQ(FormatChecksum(std::string(3, '\xff')), "253");  // 765 % 256
    EXPECT_EQ(FormatChecksum(std::string(256, '\x01')), "000");
}

TEST(FixTimestamp, FormatsUtcWithMillisecondPrecision) {
    // 1700000000.123 epoch seconds is 2023-11-14T22:13:20.123Z.
    EXPECT_EQ(FormatUtcTimestamp(1'700'000'000'123'000'000ULL), "20231114-22:13:20.123");
}

TEST(FixParser, RoundTripsEveryFieldInOrder) {
    const std::array<Field, 4> body = {
        Field{.tag = tag::kMdReqId, .value = "req-1"},
        Field{.tag = tag::kNoMdEntryTypes, .value = "2"},
        Field{.tag = tag::kMdEntryType, .value = "0"},
        Field{.tag = tag::kMdEntryType, .value = "1"},
    };
    const std::string message = BuildMessage(HeaderFor("V", 9), body);

    const auto parsed = ParseMessage(message);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->MsgType(), "V");

    const std::vector<std::pair<int, std::string_view>> expected = {
        {tag::kMsgType, "V"},
        {tag::kSenderCompId, "A"},
        {tag::kTargetCompId, "B"},
        {tag::kMsgSeqNum, "9"},
        {tag::kSendingTime, "20260101-00:00:00.000"},
        {tag::kMdReqId, "req-1"},
        {tag::kNoMdEntryTypes, "2"},
        {tag::kMdEntryType, "0"},
        {tag::kMdEntryType, "1"},
    };
    ASSERT_EQ(parsed->BodyFields().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(parsed->BodyFields()[index].tag, expected[index].first) << "at " << index;
        EXPECT_EQ(parsed->BodyFields()[index].value, expected[index].second) << "at " << index;
    }

    // fields() additionally carries the envelope, body_fields() does not.
    EXPECT_EQ(parsed->size(), expected.size() + 3);
    EXPECT_EQ(parsed->Fields().front().tag, tag::kBeginString);
    EXPECT_EQ(parsed->Fields().back().tag, tag::kCheckSum);
}

TEST(FixParser, GetReturnsTheFirstOccurrenceOfARepeatedTag) {
    // Unchanged by the group reader and deliberately so: get() answers about
    // the message, so for a tag that repeats inside a group its answer is
    // arbitrary. A group member is reached through ReadGroup(), never here.
    const std::array<Field, 3> body = {
        Field{.tag = tag::kNoMdEntryTypes, .value = "2"},
        Field{.tag = tag::kMdEntryType, .value = "0"},
        Field{.tag = tag::kMdEntryType, .value = "1"},
    };
    const auto parsed = ParseMessage(BuildMessage(HeaderFor("V", 1), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->Count(tag::kMdEntryType), 2U);
    EXPECT_EQ(parsed->Get(tag::kMdEntryType), "0");
}

TEST(FixParser, SplitsOnlyTheTagOnEqualsSoBase64ValuesSurvive) {
    // RawData is base64 and routinely ends in '='. Splitting a field on every
    // '=' would truncate the Logon password and the session would be rejected
    // for reasons that look nothing like a parser bug.
    constexpr std::string_view kRawData = "1700000000000.AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBka=";
    const std::array<Field, 1> body = {
        Field{.tag = tag::kRawData, .value = std::string(kRawData)},
    };
    const auto parsed = ParseMessage(BuildMessage(HeaderFor("A", 1), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->Get(tag::kRawData), kRawData);
}

TEST(FixParser, ParsesIntegerFieldsAndRejectsNonIntegers) {
    const std::array<Field, 1> body = {Field{.tag = tag::kText, .value = "12abc"}};
    const auto parsed = ParseMessage(BuildMessage(HeaderFor("0", 4242), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->GetInt(tag::kMsgSeqNum), 4242);
    EXPECT_FALSE(parsed->GetInt(tag::kText).has_value());
    EXPECT_FALSE(parsed->GetInt(tag::kSymbol).has_value());
}

TEST(FixParser, RejectsACorruptedCheckSum) {
    std::string message = Wire(kHandVerified);
    message[message.size() - 2] = '3';  // "10=052" -> "10=053"
    const auto parsed = ParseMessage(message);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("CheckSum"), std::string::npos) << parsed.error();
}

TEST(FixParser, RejectsAWrongBodyLength) {
    // Same width, so nothing else about the message moves: only the claim is
    // wrong.
    std::string message = Wire(kHandVerified);
    message.replace(message.find("9=45"), 4, "9=44");
    const auto parsed = ParseMessage(message);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().find("BodyLength"), std::string::npos) << parsed.error();
}

TEST(FixParser, RejectsMalformedEnvelopes) {
    EXPECT_FALSE(ParseMessage("").has_value());
    EXPECT_FALSE(ParseMessage(Wire("35=0|49=A|10=000|")).has_value());
    EXPECT_FALSE(ParseMessage(Wire("8=FIX.4.2|9=45|35=0|10=052|")).has_value());
    EXPECT_FALSE(ParseMessage(Wire("8=FIX.4.4|35=0|49=A|10=052|")).has_value());
    // MsgType must lead the body.
    EXPECT_FALSE(ParseMessage(Wire("8=FIX.4.4|9=10|49=A|35=0|10=000|")).has_value());
    // No CheckSum at all.
    EXPECT_FALSE(ParseMessage(Wire("8=FIX.4.4|9=5|35=0|")).has_value());
    // Trailing bytes after CheckSum.
    EXPECT_FALSE(ParseMessage(Wire(kHandVerified) + "junk").has_value());
}

TEST(FixFramer, SplitsSeveralMessagesOutOfOneChunk) {
    Framer stream;
    stream.Append(Wire(kHandVerified) + Wire(kHandVerified));

    ASSERT_TRUE(stream.NextMessage().has_value());
    ASSERT_TRUE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_TRUE(stream.Good());
    EXPECT_EQ(stream.MessagesFramed(), 2U);
    EXPECT_EQ(stream.BufferedBytes(), 0U);
}

TEST(FixFramer, ReassemblesAMessageDeliveredOneByteAtATime) {
    // The case a real socket produces and a "split on SOH" framer gets wrong.
    const std::string message = Wire(kHandVerified);
    Framer stream;
    for (std::size_t index = 0; index + 1 < message.size(); ++index) {
        stream.Append(message.substr(index, 1));
        EXPECT_FALSE(stream.NextMessage().has_value()) << "completed early at byte " << index;
        EXPECT_TRUE(stream.Good());
    }
    stream.Append(message.substr(message.size() - 1));

    const auto framed = stream.NextMessage();
    ASSERT_TRUE(framed.has_value());
    EXPECT_EQ(*framed, message);
}

TEST(FixFramer, KeepsAPartialTrailingMessageBufferedForNextTime) {
    const std::string message = Wire(kHandVerified);
    Framer stream;
    stream.Append(message + message.substr(0, 20));

    ASSERT_TRUE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_EQ(stream.BufferedBytes(), 20U);

    stream.Append(message.substr(20));
    const auto second = stream.NextMessage();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, message);
}

TEST(FixFramer, DoesNotSplitOnATenEqualsInsideAFieldValue) {
    // The whole reason the framer trusts BodyLength instead of scanning for
    // "10=": that byte sequence occurs naturally inside prices and ids.
    const std::array<Field, 1> body = {Field{.tag = tag::kText, .value = "x10=y"}};
    const std::string message = BuildMessage(HeaderFor("0", 1), body);
    Framer stream;
    stream.Append(message);

    const auto framed = stream.NextMessage();
    ASSERT_TRUE(framed.has_value());
    EXPECT_EQ(*framed, message);
    EXPECT_TRUE(ParseMessage(*framed).has_value());
}

TEST(FixFramer, AcceptsRawBytesAsWellAsText) {
    const std::string message = Wire(kHandVerified);
    Framer stream;
    stream.Append(BytesOf(message));
    EXPECT_TRUE(stream.NextMessage().has_value());
}

TEST(FixFramer, FailsStickilyOnAStreamThatIsNotFix) {
    Framer stream;
    stream.Append("GET / HTTP/1.1\r\n");
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.Good());
    EXPECT_NE(stream.Error().find("8=FIX.4.4"), std::string::npos) << stream.Error();

    // Sticky: feeding it valid data afterwards must not silently resume, since
    // everything after a framing loss is unaligned.
    stream.Append(Wire(kHandVerified));
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.Good());
}

TEST(FixFramer, WaitsRatherThanFailingOnAPartialBeginString) {
    Framer stream;
    stream.Append("8=FIX");
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_TRUE(stream.Good());
}

TEST(FixFramer, RefusesAnAbsurdBodyLength) {
    Framer stream;
    stream.Append(Wire("8=FIX.4.4|9=99999999|"));
    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.Good());
    EXPECT_NE(stream.Error().find("maximum"), std::string::npos) << stream.Error();
}

TEST(FixFramer, RefusesABodyLengthThatDoesNotLandOnACheckSumField) {
    std::string message = Wire(kHandVerified);
    message.replace(message.find("9=45"), 4, "9=40");
    Framer stream;
    stream.Append(message);

    EXPECT_FALSE(stream.NextMessage().has_value());
    EXPECT_FALSE(stream.Good());
    EXPECT_NE(stream.Error().find("CheckSum"), std::string::npos) << stream.Error();
}

TEST(FixFramer, LeavesCheckSumValidationToTheParser) {
    // Framing is a transport concern; message validity is not. A message whose
    // bytes are correctly delimited but whose CheckSum is wrong is framed out
    // successfully and rejected on parse -- that split is deliberate, so the
    // client can decide what a bad CheckSum means for the session.
    std::string message = Wire(kHandVerified);
    message[message.size() - 2] = '3';
    Framer stream;
    stream.Append(message);

    const auto framed = stream.NextMessage();
    ASSERT_TRUE(framed.has_value());
    EXPECT_TRUE(stream.Good());
    EXPECT_FALSE(ParseMessage(*framed).has_value());
}

TEST(FixGroup, ReadsEveryEntryOfASnapshotInOrder) {
    const std::array<MdEntry, 3> entries = {
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "64000.5",
                .size = "10",
                .date = "20260916"},
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "63999.0",
                .size = "250",
                .date = "20260916"},
        MdEntry{.update_action = "",
                .entry_type = "1",
                .price = "64001.0",
                .size = "7",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("W", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), entries.size());
    EXPECT_EQ(group->declared_count, entries.size());
    EXPECT_FALSE(group->Truncated());

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = (*group)[index];
        EXPECT_EQ(entry.Get(tag::kMdEntryType), entries[index].entry_type) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntryPx), entries[index].price) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntrySize), entries[index].size) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntryDate), entries[index].date) << "at " << index;
        // A snapshot entry has no MDUpdateAction: absent, not an error.
        EXPECT_FALSE(entry.Get(tag::kMdUpdateAction).has_value()) << "at " << index;
        EXPECT_EQ(entry.size(), kSnapshotEntryTags.size());
    }
    EXPECT_EQ((*group)[1].GetInt(tag::kMdEntrySize), 250);
}

TEST(FixGroup, ReadsIncrementalEntriesWithTheirUpdateActions) {
    // 0 = New, 1 = Change, 2 = Delete -- all three observed live
    // (exchanges/deribit.md).
    const std::array<MdEntry, 3> entries = {
        MdEntry{.update_action = "0",
                .entry_type = "0",
                .price = "64000.5",
                .size = "10",
                .date = "20260916"},
        MdEntry{.update_action = "1",
                .entry_type = "1",
                .price = "64001.0",
                .size = "3",
                .date = "20260916"},
        MdEntry{.update_action = "2",
                .entry_type = "0",
                .price = "63998.0",
                .size = "0",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 3U);

    std::vector<std::int64_t> actions;
    for (const auto& entry : *group) {
        const auto action = entry.GetInt(tag::kMdUpdateAction);
        ASSERT_TRUE(action.has_value());
        actions.push_back(*action);
        // 279 leads an incremental entry, so it is this group's delimiter.
        EXPECT_EQ(entry.Fields().front().tag, tag::kMdUpdateAction);
        EXPECT_EQ(entry.size(), kIncrementalEntryTags.size());
    }
    EXPECT_EQ(actions, (std::vector<std::int64_t>{0, 1, 2}));
    EXPECT_EQ((*group)[1].Get(tag::kMdEntryPx), "64001.0");
    EXPECT_EQ((*group)[2].Get(tag::kMdEntrySize), "0");
}

TEST(FixGroup, FindsBoundariesByTheDelimiterTagNotByAFixedStride) {
    // The entry in the middle omits MDEntryDate(272). A reader that assumed
    // four fields per entry would shift every later entry by one field and
    // report the wrong prices -- silently, and only on some messages.
    const std::array<MdEntry, 3> entries = {
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
        MdEntry{.update_action = "", .entry_type = "0", .price = "2.0", .size = "2", .date = ""},
        MdEntry{.update_action = "",
                .entry_type = "1",
                .price = "3.0",
                .size = "3",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("W", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 3U);
    EXPECT_EQ((*group)[0].Get(tag::kMdEntryPx), "1.0");
    EXPECT_EQ((*group)[1].Get(tag::kMdEntryPx), "2.0");
    EXPECT_FALSE((*group)[1].Get(tag::kMdEntryDate).has_value());
    EXPECT_EQ((*group)[1].size(), 3U);
    EXPECT_EQ((*group)[2].Get(tag::kMdEntryPx), "3.0");
    EXPECT_EQ((*group)[2].Get(tag::kMdEntryDate), "20260916");
}

TEST(FixGroup, ReadsASingleEntryGroup) {
    // The commonest incremental refresh on the wire carries exactly one entry.
    const std::array<MdEntry, 1> entries = {
        MdEntry{.update_action = "1",
                .entry_type = "0",
                .price = "64000.5",
                .size = "12",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 1U);
    EXPECT_FALSE(group->Truncated());
    EXPECT_EQ(group->entries.front().Get(tag::kMdEntrySize), "12");
}

TEST(FixGroup, ReadsAZeroEntryGroupAsEmptyRatherThanAnError) {
    const auto parsed = ParseMessage(MdMessage("X", std::span<const MdEntry>{}));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->Get(tag::kNoMdEntries), "0");

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_TRUE(group->empty());
    EXPECT_EQ(group->declared_count, 0U);
    EXPECT_FALSE(group->Truncated());
}

TEST(FixGroup, StopsAtTheFirstTagThatIsNotAGroupMember) {
    const std::array<MdEntry, 2> entries = {
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
        MdEntry{.update_action = "",
                .entry_type = "1",
                .price = "2.0",
                .size = "2",
                .date = "20260916"},
    };
    const std::array<Field, 1> trailing = {Field{.tag = tag::kText, .value = "after the group"}};
    const auto parsed = ParseMessage(MdMessage("W", entries, std::nullopt, trailing));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_EQ(group->entries.back().size(), kSnapshotEntryTags.size());
    EXPECT_EQ(parsed->Get(tag::kText), "after the group");
}

TEST(FixGroup, SalvagesTheEntriesAMessageActuallyCarriesWhenNumInGroupLies) {
    // NoMDEntries claims five, two are present: return the two and say the
    // message was short, rather than reading past the end of the field list or
    // throwing away data the caller could still inspect.
    const std::array<MdEntry, 2> entries = {
        MdEntry{.update_action = "0",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
        MdEntry{.update_action = "2",
                .entry_type = "1",
                .price = "2.0",
                .size = "0",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("X", entries, 5));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kIncrementalEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_EQ(group->declared_count, 5U);
    EXPECT_TRUE(group->Truncated());
    EXPECT_EQ((*group)[0].Get(tag::kMdEntryPx), "1.0");
    EXPECT_EQ((*group)[1].Get(tag::kMdEntryPx), "2.0");
}

TEST(FixGroup, SurvivesAnAbsurdNumInGroupWithoutAllocatingForIt) {
    // The out-of-memory shape of the same lie: nothing may be sized from the
    // declared count. A hang or a bad_alloc here is the failure.
    const std::array<MdEntry, 1> entries = {
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("W", entries, 1'000'000'000));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_EQ(group->size(), 1U);
    EXPECT_TRUE(group->Truncated());
}

TEST(FixGroup, TreatsNumInGroupAsAuthoritativeWhenMoreEntriesAreOnTheWire) {
    // Three entries, a count of two: the third is outside the group as far as
    // the message's own declaration is concerned, and must not be swept in.
    const std::array<MdEntry, 3> entries = {
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
        MdEntry{.update_action = "",
                .entry_type = "0",
                .price = "2.0",
                .size = "2",
                .date = "20260916"},
        MdEntry{.update_action = "",
                .entry_type = "1",
                .price = "3.0",
                .size = "3",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("W", entries, 2));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_EQ(group->size(), 2U);
    EXPECT_FALSE(group->Truncated());
    EXPECT_EQ((*group)[1].Get(tag::kMdEntryPx), "2.0");
}

TEST(FixGroup, ReadsNothingWhenAskedForTheWrongEntryShape) {
    // 35=X entries open with 279, which the 35=W member set does not contain,
    // so the group is reported empty-and-short rather than mis-parsed. The
    // shape belongs to the call, not to a guess made inside the reader.
    const std::array<MdEntry, 2> entries = {
        MdEntry{.update_action = "0",
                .entry_type = "0",
                .price = "1.0",
                .size = "1",
                .date = "20260916"},
        MdEntry{.update_action = "1",
                .entry_type = "1",
                .price = "2.0",
                .size = "2",
                .date = "20260916"},
    };
    const auto parsed = ParseMessage(MdMessage("X", entries));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto group = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_TRUE(group.has_value()) << group.error();
    EXPECT_TRUE(group->empty());
    EXPECT_TRUE(group->Truncated());
}

TEST(FixGroup, ReadsTheOtherGroupsAMarketDataRequestCarries) {
    // NoMDEntryTypes(267) is a one-member group -- delimiter and entry are the
    // same tag -- and the reader must not need a second member to find its
    // boundaries.
    const std::array<Field, 5> body = {
        Field{.tag = tag::kNoMdEntryTypes, .value = "2"},
        Field{.tag = tag::kMdEntryType, .value = "0"},
        Field{.tag = tag::kMdEntryType, .value = "1"},
        Field{.tag = tag::kNoRelatedSym, .value = "1"},
        Field{.tag = tag::kSymbol, .value = "BTC-PERPETUAL"},
    };
    const auto parsed = ParseMessage(BuildMessage(HeaderFor("V", 3), body));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto types = ReadGroup(*parsed, tag::kNoMdEntryTypes, {tag::kMdEntryType});
    ASSERT_TRUE(types.has_value()) << types.error();
    ASSERT_EQ(types->size(), 2U);
    EXPECT_EQ((*types)[0].Get(tag::kMdEntryType), "0");
    EXPECT_EQ((*types)[1].Get(tag::kMdEntryType), "1");

    const auto symbols = ReadGroup(*parsed, tag::kNoRelatedSym, {tag::kSymbol});
    ASSERT_TRUE(symbols.has_value()) << symbols.error();
    ASSERT_EQ(symbols->size(), 1U);
    EXPECT_EQ(symbols->entries.front().Get(tag::kSymbol), "BTC-PERPETUAL");
}

TEST(FixGroup, ReadsFromAParsedMessageHeldInANamedVariable) {
    // The lifetime-safe spelling, pinned so the rvalue overload of ReadGroup
    // being `= delete`d cannot quietly cost the legitimate idiom anything.
    //
    // A repeating_group is spans all the way down: into the ParsedMessage's
    // field list, which is views into the parsed bytes. Both have to outlive
    // the group, so both are named variables here and the group is read and
    // then used across later statements -- long after the full expression that
    // built it would have destroyed a temporary.
    const std::array<MdEntry, 2> entries = {
        MdEntry{.update_action = "0",
                .entry_type = "0",
                .price = "64000.5",
                .size = "10",
                .date = "20260916"},
        MdEntry{.update_action = "2",
                .entry_type = "1",
                .price = "64001.0",
                .size = "0",
                .date = "20260916"},
    };
    const std::string raw = MdMessage("X", entries);
    const auto parsed = ParseMessage(raw);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    // Both lvalue spellings a call site can reach for: straight through the
    // expected, and through a named ParsedMessage reference.
    const ParsedMessage& message = *parsed;
    const auto group = ReadGroup(message, tag::kNoMdEntries, kIncrementalEntryTags);
    const auto same_group = ReadGroup(*parsed, tag::kNoMdEntries,
                                      {tag::kMdUpdateAction, tag::kMdEntryType, tag::kMdEntryPx,
                                       tag::kMdEntrySize, tag::kMdEntryDate});
    ASSERT_TRUE(group.has_value()) << group.error();
    ASSERT_TRUE(same_group.has_value()) << same_group.error();
    ASSERT_EQ(group->size(), entries.size());
    ASSERT_EQ(same_group->size(), entries.size());
    EXPECT_FALSE(group->Truncated());

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = (*group)[index];
        EXPECT_EQ(entry.Get(tag::kMdUpdateAction), entries[index].update_action) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntryType), entries[index].entry_type) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntryPx), entries[index].price) << "at " << index;
        EXPECT_EQ(entry.Get(tag::kMdEntrySize), entries[index].size) << "at " << index;
        EXPECT_EQ((*same_group)[index].Get(tag::kMdEntryPx), entries[index].price)
            << "at " << index;
    }

    // Still readable here, several statements on: the views point into `raw`
    // and `parsed`, both of which are still alive.
    EXPECT_EQ(group->entries.front().Fields().front().tag, tag::kMdUpdateAction);
    EXPECT_EQ((*group)[1].GetInt(tag::kMdEntrySize), 0);
}

TEST(FixGroup, FailsOnlyWhenThereIsNoGroupToRead) {
    const auto parsed = ParseMessage(Wire(kHandVerified));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    const auto missing = ReadGroup(*parsed, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("NumInGroup"), std::string::npos) << missing.error();

    const std::array<Field, 2> body = {
        Field{.tag = tag::kNoMdEntries, .value = "two"},
        Field{.tag = tag::kMdEntryType, .value = "0"},
    };
    const auto bad_count = ParseMessage(BuildMessage(HeaderFor("X", 1), body));
    ASSERT_TRUE(bad_count.has_value()) << bad_count.error();
    const auto unparsable = ReadGroup(*bad_count, tag::kNoMdEntries, kSnapshotEntryTags);
    ASSERT_FALSE(unparsable.has_value());
    EXPECT_NE(unparsable.error().find("non-negative"), std::string::npos) << unparsable.error();
}
