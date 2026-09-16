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
inline constexpr int begin_string = 8;
inline constexpr int body_length = 9;
inline constexpr int check_sum = 10;
inline constexpr int msg_type = 35;
inline constexpr int msg_seq_num = 34;
inline constexpr int sender_comp_id = 49;
inline constexpr int target_comp_id = 56;
inline constexpr int sending_time = 52;
inline constexpr int poss_dup_flag = 43;
inline constexpr int test_req_id = 112;
inline constexpr int encrypt_method = 98;
inline constexpr int heart_bt_int = 108;
inline constexpr int raw_data = 96;
inline constexpr int username = 553;
inline constexpr int password = 554;
inline constexpr int text = 58;
inline constexpr int symbol = 55;
inline constexpr int no_related_sym = 146;
inline constexpr int md_req_id = 262;
inline constexpr int subscription_request_type = 263;
inline constexpr int market_depth = 264;
inline constexpr int no_md_entry_types = 267;
inline constexpr int no_md_entries = 268;
inline constexpr int md_entry_type = 269;
inline constexpr int md_entry_px = 270;
inline constexpr int md_entry_size = 271;
inline constexpr int md_entry_date = 272;
inline constexpr int md_update_action = 279;
}  // namespace tag

/// The message types this project sends or reacts to (exchanges/deribit.md).
namespace msg_type {
inline constexpr std::string_view heartbeat = "0";
inline constexpr std::string_view test_request = "1";
inline constexpr std::string_view resend_request = "2";
inline constexpr std::string_view reject = "3";
inline constexpr std::string_view sequence_reset = "4";
inline constexpr std::string_view logout = "5";
inline constexpr std::string_view logon = "A";
inline constexpr std::string_view market_data_request = "V";
inline constexpr std::string_view market_data_request_reject = "Y";
inline constexpr std::string_view market_data_snapshot_full_refresh = "W";
inline constexpr std::string_view market_data_incremental_refresh = "X";
}  // namespace msg_type

/// One outbound field. Values are owned `std::string`s rather than views
/// because most of them (sequence numbers, timestamps, the Logon password) are
/// computed at build time; session/control messages are not a hot path, so the
/// copies are not worth the lifetime hazard of holding views.
struct field {
    int tag = 0;
    std::string value;
};

/// The standard header fields every outbound session message carries, in the
/// order the probe (and therefore Deribit) sends them.
struct session_header {
    std::string_view msg_type;        // 35
    std::string_view sender_comp_id;  // 49
    std::string_view target_comp_id;  // 56
    std::uint64_t msg_seq_num = 0;    // 34
    std::string_view sending_time;    // 52
};

/// Wraps `fields` -- already in wire order and starting with MsgType(35) -- in
/// the BeginString/BodyLength prefix and CheckSum trailer. The generic bottom
/// layer: it knows the envelope arithmetic and nothing else.
std::string build_message(std::span<const field> fields);

/// Renders `header` into its standard fields, appends `body`, and wraps the
/// result. This is the entry point exchange-specific code uses.
std::string build_message(const session_header& header, std::span<const field> body);

/// FIX UTCTimestamp (tag 52) for `realtime_ns`: "YYYYMMDD-HH:MM:SS.sss", UTC,
/// millisecond precision -- the same precision simplefix's
/// append_utc_timestamp produced in the probe Deribit accepted.
std::string format_utc_timestamp(std::uint64_t realtime_ns);

/// CheckSum of every byte in `bytes`, as the three zero-padded digits that go
/// in tag 10. Exposed for tests that verify the arithmetic directly.
std::string format_checksum(std::string_view bytes);

/// A parsed message's fields, in wire order.
///
/// Lifetime: every value is a view into the buffer `parse_message` was handed
/// (the same non-owning convention as `capture_frame`, decisions/0004). When
/// that buffer is a `framer`'s, it stays valid only until the next
/// append()/next_message() call.
///
/// The field list stays flat and ordered rather than being turned into a tree:
/// `get()` returns the FIRST occurrence of a tag, which is right for the
/// top-level session fields and meaningless for a tag that repeats inside a
/// group. Repeating groups are read on top of this ordered list by
/// `read_group()` below -- that is what the ordering is preserved for -- so a
/// group member is reached through a `group_entry`, never through `get()`.
///
/// Still not supported, deliberately: nested groups (a group whose member is
/// itself a NumInGroup field). `read_group()` handles one flat group, which is
/// the shape every group this project meets actually has (exchanges/deribit.md:
/// NoMDEntries, NoMDEntryTypes, NoRelatedSym). Building the general
/// group-of-groups walker would need a data dictionary to know which tags nest,
/// and nothing here has a use for one.
class parsed_message {
  public:
    struct field_view {
        int tag = 0;
        std::string_view value;
    };

    /// Every field in wire order, including BeginString(8), BodyLength(9) and
    /// CheckSum(10).
    std::span<const field_view> fields() const {
        return fields_;
    }

    /// The fields between BodyLength and CheckSum, i.e. starting at MsgType.
    std::span<const field_view> body_fields() const;

    /// First value carrying `tag`, or nullopt. See the repeating-group caveat
    /// above: for a tag that repeats inside a group, use `read_group()`.
    std::optional<std::string_view> get(int tag) const;

    /// First value carrying `tag`, parsed as a decimal integer. nullopt if the
    /// tag is absent or its value is not a well-formed integer.
    std::optional<std::int64_t> get_int(int tag) const;

    std::size_t count(int tag) const;

    /// Value of MsgType(35). Never empty on a successfully parsed message.
    std::string_view msg_type() const {
        return msg_type_;
    }

    std::size_t size() const {
        return fields_.size();
    }

  private:
    friend std::expected<parsed_message, std::string> parse_message(std::string_view raw);

    std::vector<field_view> fields_;
    std::string_view msg_type_;
};

/// Parses one complete message, validating the envelope: BeginString must be
/// FIX.4.4, the declared BodyLength must match the actual distance to the
/// CheckSum field, MsgType must be the first body field, and the CheckSum must
/// match a recomputation. A message failing any of those is corrupt and comes
/// back as an error rather than as partially-trusted fields.
std::expected<parsed_message, std::string> parse_message(std::string_view raw);

/// One repetition of a repeating group: a view over exactly the fields that
/// belong to that repetition, in wire order.
///
/// Lookups are scoped to the repetition, which is the entire point -- the same
/// tag means a different thing in every entry, so a message-wide `get()` cannot
/// answer "this entry's MDEntryPx". A tag missing from this particular entry is
/// nullopt rather than an error, because entries in a real group are not all
/// the same shape: Deribit puts MDUpdateAction(279) on every 35=X entry and on
/// no 35=W entry (exchanges/deribit.md), and even within one message an
/// optional member may simply be absent.
///
/// Lifetime: a view into the `parsed_message` the group was read from, which is
/// itself a view into the buffer that was parsed. Both must outlive it.
class group_entry {
  public:
    group_entry() = default;
    explicit group_entry(std::span<const parsed_message::field_view> fields) : fields_(fields) {}

    /// This entry's fields, in wire order, starting with the delimiter tag.
    std::span<const parsed_message::field_view> fields() const {
        return fields_;
    }

    /// First value carrying `tag` within this entry, or nullopt.
    std::optional<std::string_view> get(int tag) const;

    /// As `get`, parsed as a decimal integer. nullopt if the tag is absent from
    /// this entry or its value is not a well-formed integer.
    std::optional<std::int64_t> get_int(int tag) const;

    std::size_t size() const {
        return fields_.size();
    }

  private:
    std::span<const parsed_message::field_view> fields_;
};

/// The entries of one repeating group, in wire order.
struct repeating_group {
    std::vector<group_entry> entries;

    /// What NumInGroup claimed. Equal to `entries.size()` on a well-formed
    /// message; larger when the message lied (see `read_group`).
    std::size_t declared_count = 0;

    /// True when fewer repetitions were actually on the wire than NumInGroup
    /// claimed -- i.e. the message is malformed and `entries` is what could be
    /// salvaged from it.
    bool truncated() const {
        return entries.size() < declared_count;
    }

    std::size_t size() const {
        return entries.size();
    }
    bool empty() const {
        return entries.empty();
    }
    const group_entry& operator[](std::size_t index) const {
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
/// A free function rather than a `parsed_message` member on purpose: the parser
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
/// entries that *are* there with `truncated()` true, never an out-of-bounds
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
std::expected<repeating_group, std::string> read_group(const parsed_message& message, int count_tag,
                                                       std::span<const int> member_tags);

/// Convenience overload so call sites can write the member tags inline:
/// `read_group(msg, tag::no_md_entries, {tag::md_update_action, ...})`.
/// std::span is not constructible from a braced list until C++26.
inline std::expected<repeating_group, std::string> read_group(
    const parsed_message& message, int count_tag, std::initializer_list<int> member_tags) {
    return read_group(message, count_tag,
                      std::span<const int>(member_tags.begin(), member_tags.size()));
}

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
class framer {
  public:
    explicit framer(std::size_t max_body_length = kMaxBodyLength);

    /// Appends bytes read from the socket. May invalidate any view previously
    /// returned by next_message().
    void append(std::string_view bytes);
    void append(std::span<const std::byte> bytes);

    /// The next complete message, as a view into this framer's buffer, valid
    /// only until the next append()/next_message() call. nullopt means "need
    /// more bytes" -- or, if good() is false, "this stream is unusable".
    ///
    /// The returned bytes are structurally delimited but NOT yet validated:
    /// run parse_message() on them, which is where BodyLength and CheckSum are
    /// checked. Framing is a transport concern, message validity is not.
    std::optional<std::string_view> next_message();

    /// False once the stream stopped looking like FIX. Sticky.
    bool good() const {
        return error_.empty();
    }

    /// Empty while good(). Never contains payload bytes -- only a description
    /// of what was structurally wrong.
    const std::string& error() const {
        return error_;
    }

    /// Bytes held but not yet returned as a message.
    std::size_t buffered_bytes() const {
        return buffer_.size() - consumed_;
    }

    std::uint64_t messages_framed() const {
        return messages_framed_;
    }

  private:
    void fail(std::string reason);
    void compact();

    std::string buffer_;
    std::string error_;
    std::size_t consumed_ = 0;
    std::size_t max_body_length_;
    std::uint64_t messages_framed_ = 0;
};

}  // namespace feed_handler::fix
