// Generic FIX.4.4 tag=value wire mechanics: building a message with a correct
// BodyLength/CheckSum envelope, framing a byte stream back into whole
// messages, and parsing one message into its fields.
//
// Hand-rolled rather than QuickFIX on purpose (decisions/0001, decisions/0002):
// the protocol layer is the exercise, in the same spirit as the hand-rolled
// RFC6455 client that is meant to replace IXWebSocket later.
//
// Nothing here is Deribit-specific and nothing here does I/O -- a socket is
// the next task's problem (decisions/0004). The framer is written for how a
// socket actually delivers data (arbitrary chunk boundaries, several messages
// per read, a partial message left over) so that plugging one in later is
// wiring, not a redesign.
//
// Wire envelope, per the FIX spec:
//
//   8=FIX.4.4<SOH> 9=<BodyLength><SOH> 35=<MsgType><SOH> ... 10=<CheckSum><SOH>
//
// * BodyLength(9) counts the bytes from immediately AFTER the SOH that
//   terminates the BodyLength field itself, up to and including the SOH that
//   terminates the last field before CheckSum. So it covers neither the
//   `8=`/`9=` fields nor the `10=` field: it is exactly the length of the
//   middle section. Its own digit count therefore cannot influence its value,
//   which is why the body is rendered first and the envelope prepended after.
// * CheckSum(10) is the sum of every byte of the message up to and including
//   that same SOH before `10=`, modulo 256, rendered as exactly three
//   zero-padded decimal digits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace feed_handler::fix {

/// Field separator. Real FIX is delimited by this non-printable byte, not by
/// the '|' that documentation and log dumps usually substitute for it.
inline constexpr char kSoh = '\x01';

inline constexpr std::string_view kBeginString = "FIX.4.4";

/// A CheckSum field is always exactly "10=NNN<SOH>" -- three digits, no more,
/// no fewer -- which is what lets the framer compute a message's total length
/// from BodyLength alone.
inline constexpr std::size_t kCheckSumFieldSize = 7;

/// Refuses to buffer more than this much body from a stream that claims a huge
/// BodyLength. Without a cap, one corrupt length field turns into unbounded
/// memory growth on a connection that will never produce a valid message
/// again. Comfortably above a full-book MarketDataSnapshotFullRefresh.
inline constexpr std::size_t kMaxBodyLength = 1U << 20U;

/// The tag numbers this project actually uses. Named rather than spelled as
/// literals at every call site: a transposed tag number is otherwise a silent
/// wire bug that only the exchange notices.
namespace tag {
inline constexpr int kBeginString = 8;
inline constexpr int kBodyLength = 9;
inline constexpr int kCheckSum = 10;
inline constexpr int kMsgType = 35;
inline constexpr int kMsgSeqNum = 34;
inline constexpr int kSenderCompId = 49;
inline constexpr int kTargetCompId = 56;
inline constexpr int kSendingTime = 52;
inline constexpr int kPossDupFlag = 43;
inline constexpr int kTestReqId = 112;
inline constexpr int kEncryptMethod = 98;
inline constexpr int kHeartBtInt = 108;
inline constexpr int kRawData = 96;
inline constexpr int kUsername = 553;
inline constexpr int kPassword = 554;
inline constexpr int kText = 58;
inline constexpr int kSymbol = 55;
inline constexpr int kNoRelatedSym = 146;
inline constexpr int kMdReqId = 262;
inline constexpr int kSubscriptionRequestType = 263;
inline constexpr int kMarketDepth = 264;
inline constexpr int kNoMdEntryTypes = 267;
inline constexpr int kNoMdEntries = 268;
inline constexpr int kMdEntryType = 269;
inline constexpr int kMdEntryPx = 270;
inline constexpr int kMdEntrySize = 271;
inline constexpr int kMdEntryDate = 272;
inline constexpr int kMdUpdateAction = 279;
}  // namespace tag

/// The message types this project sends or reacts to (exchanges/deribit.md).
namespace msg_type {
inline constexpr std::string_view kHeartbeat = "0";
inline constexpr std::string_view kTestRequest = "1";
inline constexpr std::string_view kResendRequest = "2";
inline constexpr std::string_view kReject = "3";
inline constexpr std::string_view kSequenceReset = "4";
inline constexpr std::string_view kLogout = "5";
inline constexpr std::string_view kLogon = "A";
inline constexpr std::string_view kMarketDataRequest = "V";
inline constexpr std::string_view kMarketDataRequestReject = "Y";
inline constexpr std::string_view kMarketDataSnapshotFullRefresh = "W";
inline constexpr std::string_view kMarketDataIncrementalRefresh = "X";
}  // namespace msg_type

/// One outbound field. Values are owned `std::string`s rather than views
/// because most of them (sequence numbers, timestamps, the Logon password) are
/// computed at build time; session/control messages are not a hot path, so the
/// copies are not worth the lifetime hazard of holding views.
struct Field {
    int tag = 0;
    std::string value;
};

/// The standard header fields every outbound session message carries, in the
/// order the probe (and therefore Deribit) sends them.
struct SessionHeader {
    std::string_view msg_type;        // 35
    std::string_view sender_comp_id;  // 49
    std::string_view target_comp_id;  // 56
    std::uint64_t msg_seq_num = 0;    // 34
    std::string_view sending_time;    // 52
};

/// Wraps `fields` -- already in wire order and starting with MsgType(35) -- in
/// the BeginString/BodyLength prefix and CheckSum trailer. The generic bottom
/// layer: it knows the envelope arithmetic and nothing else.
std::string BuildMessage(std::span<const Field> fields);

/// Renders `header` into its standard fields, appends `body`, and wraps the
/// result. This is the entry point exchange-specific code uses.
std::string BuildMessage(const SessionHeader& header, std::span<const Field> body);

/// FIX UTCTimestamp (tag 52) for `realtime_ns`: "YYYYMMDD-HH:MM:SS.sss", UTC,
/// millisecond precision -- the same precision simplefix's
/// append_utc_timestamp produced in the probe Deribit accepted.
std::string FormatUtcTimestamp(std::uint64_t realtime_ns);

/// CheckSum of every byte in `bytes`, as the three zero-padded digits that go
/// in tag 10. Exposed for tests that verify the arithmetic directly.
std::string FormatChecksum(std::string_view bytes);

/// A parsed message's fields, in wire order.
///
/// Lifetime: every value is a view into the buffer `ParseMessage` was handed
/// (the same non-owning convention as `CaptureFrame`, decisions/0004). When
/// that buffer is a `Framer`'s, it stays valid only until the next
/// Append()/NextMessage() call. A `ParsedMessage` is also viewed *into* --
/// `ReadGroup()` hands back spans over this object's field list -- so it must
/// itself outlive anything read out of it, which is why a temporary
/// `ParsedMessage` is rejected at compile time (see `ReadGroup` below).
///
/// The field list stays flat and ordered rather than being turned into a tree:
/// `Get()` returns the FIRST occurrence of a tag, which is right for the
/// top-level session fields and meaningless for a tag that repeats inside a
/// group. Repeating groups are read on top of this ordered list by
/// `ReadGroup()` below -- that is what the ordering is preserved for -- so a
/// group member is reached through a `GroupEntry`, never through `Get()`.
///
/// Still not supported, deliberately: nested groups (a group whose member is
/// itself a NumInGroup field). `ReadGroup()` handles one flat group, which is
/// the shape every group this project meets actually has (exchanges/deribit.md:
/// NoMDEntries, NoMDEntryTypes, NoRelatedSym). Building the general
/// group-of-groups walker would need a data dictionary to know which tags nest,
/// and nothing here has a use for one.
class ParsedMessage {
  public:
    struct FieldView {
        int tag = 0;
        std::string_view value;
    };

    /// Every field in wire order, including BeginString(8), BodyLength(9) and
    /// CheckSum(10).
    std::span<const FieldView> Fields() const {
        return fields_;
    }

    /// The fields between BodyLength and CheckSum, i.e. starting at MsgType.
    std::span<const FieldView> BodyFields() const;

    /// First value carrying `tag`, or nullopt. See the repeating-group caveat
    /// above: for a tag that repeats inside a group, use `ReadGroup()`.
    std::optional<std::string_view> Get(int tag) const;

    /// First value carrying `tag`, parsed as a decimal integer. nullopt if the
    /// tag is absent or its value is not a well-formed integer.
    std::optional<std::int64_t> GetInt(int tag) const;

    std::size_t Count(int tag) const;

    /// Value of MsgType(35). Never empty on a successfully parsed message.
    std::string_view MsgType() const {
        return msg_type_;
    }

    std::size_t size() const {
        return fields_.size();
    }

  private:
    friend std::expected<ParsedMessage, std::string> ParseMessage(std::string_view raw);

    std::vector<FieldView> fields_;
    std::string_view msg_type_;
};

/// Parses one complete message, validating the envelope: BeginString must be
/// FIX.4.4, the declared BodyLength must match the actual distance to the
/// CheckSum field, MsgType must be the first body field, and the CheckSum must
/// match a recomputation. A message failing any of those is corrupt and comes
/// back as an error rather than as partially-trusted fields.
std::expected<ParsedMessage, std::string> ParseMessage(std::string_view raw);

/// Rejects parsing straight out of a temporary `std::string` at compile time,
/// for the same reason `ReadGroup` rejects a temporary `ParsedMessage` below.
/// The natural-looking
///
///     const auto parsed = ParseMessage(BuildMessage(header, body));
///
/// would otherwise compile without a warning and dangle: `ParseMessage` copies
/// nothing, so every field of `parsed` is a view into a buffer that dies at the
/// end of that full expression. It "works" in a normal build only because a
/// short string is stored inline and the dead bytes are usually still
/// readable -- under AddressSanitizer it is a heap-use-after-free.
///
/// This outranks the `std::string_view` overload: binding a `std::string`
/// prvalue directly to `std::string&&` is an identity conversion, while
/// reaching the `std::string_view` overload needs `std::string`'s
/// user-defined conversion operator.
///
/// If this overload is what the compiler is complaining about, hoist the
/// buffer into a named variable that outlives every use of the result:
///
///     const std::string raw = BuildMessage(header, body);  // named, outlives...
///     const auto parsed = ParseMessage(raw);               // ...this
std::expected<ParsedMessage, std::string> ParseMessage(std::string&& raw) = delete;

/// Keeps a string literal working. Without it, `ParseMessage("8=FIX.4.4...")`
/// becomes *ambiguous* rather than accepted: `const char*` reaches both
/// `std::string_view` and `std::string` through a user-defined conversion, and
/// neither is better. A literal has static storage duration and cannot dangle,
/// so the safe spelling should not pay for the guard above with a diagnostic
/// that points at the wrong problem.
inline std::expected<ParsedMessage, std::string> ParseMessage(const char* raw) {
    return ParseMessage(std::string_view(raw));
}

/// One repetition of a repeating group: a view over exactly the fields that
/// belong to that repetition, in wire order.
///
/// Lookups are scoped to the repetition, which is the entire point -- the same
/// tag means a different thing in every entry, so a message-wide `Get()` cannot
/// answer "this entry's MDEntryPx". A tag missing from this particular entry is
/// nullopt rather than an error, because entries in a real group are not all
/// the same shape: Deribit puts MDUpdateAction(279) on every 35=X entry and on
/// no 35=W entry (exchanges/deribit.md), and even within one message an
/// optional member may simply be absent.
///
/// Lifetime: a view into the `ParsedMessage` the group was read from, which is
/// itself a view into the buffer that was parsed. Both must outlive it.
class GroupEntry {
  public:
    GroupEntry() = default;
    explicit GroupEntry(std::span<const ParsedMessage::FieldView> fields) : fields_(fields) {}

    /// This entry's fields, in wire order, starting with the delimiter tag.
    std::span<const ParsedMessage::FieldView> Fields() const {
        return fields_;
    }

    /// First value carrying `tag` within this entry, or nullopt.
    std::optional<std::string_view> Get(int tag) const;

    /// As `get`, parsed as a decimal integer. nullopt if the tag is absent from
    /// this entry or its value is not a well-formed integer.
    std::optional<std::int64_t> GetInt(int tag) const;

    std::size_t size() const {
        return fields_.size();
    }

  private:
    std::span<const ParsedMessage::FieldView> fields_;
};

/// The entries of one repeating group, in wire order.
struct RepeatingGroup {
    std::vector<GroupEntry> entries;

    /// What NumInGroup claimed. Equal to `entries.size()` on a well-formed
    /// message; larger when the message lied (see `ReadGroup`).
    std::size_t declared_count = 0;

    /// True when fewer repetitions were actually on the wire than NumInGroup
    /// claimed -- i.e. the message is malformed and `entries` is what could be
    /// salvaged from it.
    bool Truncated() const {
        return entries.size() < declared_count;
    }

    std::size_t size() const {
        return entries.size();
    }
    bool empty() const {
        return entries.empty();
    }
    const GroupEntry& operator[](std::size_t index) const {
        return entries[index];
    }
    auto begin() const {
        return entries.begin();
    }
    auto end() const {
        return entries.end();
    }
};

/// Reads the flat repeating group introduced by the NumInGroup field
/// `count_tag` (e.g. NoMDEntries(268)), whose repetitions are made of
/// `member_tags`.
///
/// A free function rather than a `ParsedMessage` member on purpose: the parser
/// keeps producing one flat ordered list and knows nothing about groups, and
/// this layers on top of that list without re-parsing -- which is exactly what
/// preserving wire order buys.
///
/// How a repetition's boundary is found, and why it is not a field count: the
/// group's *delimiter* is the first tag that appears after NumInGroup, and a
/// new repetition starts at every later occurrence of it. Counting a fixed
/// number of fields per entry would desynchronise the whole group the first
/// time an exchange omits an optional member or adds one, which is a normal,
/// legal thing for it to do. `member_tags` only says where the group *ends* --
/// the first tag that is not a member terminates it -- so the two message
/// shapes Deribit sends are just two calls with different member sets, and the
/// caller never has to pretend 35=W and 35=X have the same entry layout.
///
/// Malformed input is salvaged, not rejected, and the caller is told: a
/// NumInGroup larger than the number of repetitions actually present yields the
/// entries that *are* there with `Truncated()` true, never an out-of-bounds
/// read and never an unbounded loop (nothing is sized or reserved from the
/// declared count; the scan is bounded by the field list). That mirrors the
/// framer/parser split -- hand out what was structurally recoverable, flag the
/// defect, let the client decide what it means for the session -- and leaves
/// the entries inspectable, which turning it into an error would not.
/// NumInGroup is authoritative in the other direction too: if more repetitions
/// are on the wire than it declares, only the declared ones are returned and
/// the rest are left as ordinary trailing fields, so a stray member-tagged
/// field after the group cannot be absorbed into it.
///
/// Errors (rather than empty results) are reserved for "this is not a group at
/// all": no `count_tag` field in the message, or a NumInGroup value that is not
/// a non-negative integer. A declared count of zero is a valid, empty group.
///
/// If `count_tag` occurs more than once, the first occurrence wins -- two
/// groups sharing a NumInGroup tag in one message does not happen on this wire.
///
/// Lifetime: the returned `RepeatingGroup` holds `GroupEntry`s that are spans
/// into `message`'s own field list, so `message` must outlive the group (and
/// the buffer `message` was parsed from must outlive both). Bind the parsed
/// message to a named variable first -- see the deleted rvalue overload below.
std::expected<RepeatingGroup, std::string> ReadGroup(const ParsedMessage& message, int count_tag,
                                                     std::span<const int> member_tags);

/// Rejects a temporary `ParsedMessage` at compile time. The natural-looking
///
///     auto group = ReadGroup(*ParseMessage(raw), tag::kNoMdEntries, {...});
///
/// would otherwise compile without a warning and dangle: `ParseMessage`
/// returns an `std::expected` prvalue, that temporary dies at the end of the
/// full expression, and every span inside `group` points into the field list it
/// took with it. Binding to a `const&` parameter does not extend the temporary
/// past the enclosing statement, and the group outlives the statement.
///
/// If this overload is what the compiler is complaining about, hoist the parse:
///
///     const auto parsed = ParseMessage(raw);             // named, outlives...
///     if (!parsed) { ... }
///     const auto group = ReadGroup(*parsed, tag::kNoMdEntries, {...});  // ...this
std::expected<RepeatingGroup, std::string> ReadGroup(ParsedMessage&& message, int count_tag,
                                                     std::span<const int> member_tags) = delete;

/// Convenience overload so call sites can write the member tags inline:
/// `ReadGroup(msg, tag::kNoMdEntries, {tag::kMdUpdateAction, ...})`.
/// std::span is not constructible from a braced list until C++26.
inline std::expected<RepeatingGroup, std::string> ReadGroup(
    const ParsedMessage& message, int count_tag, std::initializer_list<int> member_tags) {
    return ReadGroup(message, count_tag,
                     std::span<const int>(member_tags.begin(), member_tags.size()));
}

/// The same rvalue guard for the braced-list convenience overload: without it,
/// the inline-member-tags spelling -- which is the one a call site is most
/// likely to reach for -- would still silently dangle.
std::expected<RepeatingGroup, std::string> ReadGroup(
    ParsedMessage&& message, int count_tag, std::initializer_list<int> member_tags) = delete;

/// Reassembles whole FIX messages from a stream that arrives in arbitrary
/// chunks.
///
/// A message's end is found from BodyLength, not by searching for "10=": the
/// three-byte sequence "10=" can legitimately occur inside a field value (a
/// price, an order id), so scanning for it would split messages in the wrong
/// place. Only BodyLength says where the body actually ends.
///
/// Errors are sticky. There is no attempt to resynchronise mid-stream by
/// hunting for the next BeginString: if the framing is wrong, everything
/// after it is untrustworthy, and v1's answer to an untrustworthy session is
/// the same as decisions/0004's answer to a sequence gap -- drop the
/// connection and re-logon. That orchestration belongs to the client, so this
/// class only has to make the condition unmissable.
///
/// Not thread safe, and does not need to be: one framer per connection, driven
/// by that connection's own thread (decisions/0004, threading model).
class Framer {
  public:
    explicit Framer(std::size_t max_body_length = kMaxBodyLength);

    /// Appends bytes read from the socket. May invalidate any view previously
    /// returned by NextMessage().
    void Append(std::string_view bytes);
    void Append(std::span<const std::byte> bytes);

    /// The next complete message, as a view into this framer's buffer, valid
    /// only until the next Append()/NextMessage() call. nullopt means "need
    /// more bytes" -- or, if Good() is false, "this stream is unusable".
    ///
    /// The returned bytes are structurally delimited but NOT yet validated:
    /// run ParseMessage() on them, which is where BodyLength and CheckSum are
    /// checked. Framing is a transport concern, message validity is not.
    std::optional<std::string_view> NextMessage();

    /// False once the stream stopped looking like FIX. Sticky.
    bool Good() const {
        return error_.empty();
    }

    /// Empty while Good(). Never contains payload bytes -- only a description
    /// of what was structurally wrong.
    const std::string& Error() const {
        return error_;
    }

    /// Bytes held but not yet returned as a message.
    std::size_t BufferedBytes() const {
        return buffer_.size() - consumed_;
    }

    std::uint64_t MessagesFramed() const {
        return messages_framed_;
    }

  private:
    void Fail(std::string reason);
    void Compact();

    std::string buffer_;
    std::string error_;
    std::size_t consumed_ = 0;
    std::size_t max_body_length_;
    std::uint64_t messages_framed_ = 0;
};

}  // namespace feed_handler::fix
