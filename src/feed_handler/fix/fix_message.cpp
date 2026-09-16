#include "feed_handler/fix/fix_message.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <ctime>

namespace feed_handler::fix {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
constexpr std::uint64_t kNanosPerMilli = 1'000'000;
constexpr std::size_t kCheckSumDigits = 3;
constexpr unsigned kCheckSumModulus = 256;

/// A BodyLength longer than this many digits is garbage rather than a large
/// message, and bounds how long the framer will wait for a terminating SOH
/// that is never going to arrive.
constexpr std::size_t kMaxBodyLengthDigits = 10;

bool is_digit(char character) {
    return character >= '0' && character <= '9';
}

/// "8=FIX.4.4<SOH>" -- the fixed opening bytes of every message on this
/// session, and the only thing the framer can synchronise on.
std::string begin_string_prefix() {
    std::string prefix = "8=";
    prefix += kBeginString;
    prefix += kSoh;
    return prefix;
}

std::string checksum_field(std::string_view digits) {
    std::string rendered = "10=";
    rendered += digits;
    rendered += kSoh;
    return rendered;
}

/// A whole-value decimal integer, or nullopt. Whole-value matters: "12abc" is
/// not 12 here, it is a field this code does not understand.
std::optional<std::int64_t> to_int(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::string_view> first_value(std::span<const parsed_message::field_view> fields,
                                            int tag_number) {
    for (const auto& one : fields) {
        if (one.tag == tag_number) {
            return one.value;
        }
    }
    return std::nullopt;
}

struct scanned_field {
    int tag = 0;
    std::string_view value;
    /// Offset of the first byte after this field's terminating SOH.
    std::size_t next = 0;
};

/// Reads one "tag=value<SOH>" field starting at `offset`.
///
/// The value runs to the next SOH, not to the next '=' -- values legitimately
/// contain '=' (RawData's base64 padding is the case that actually occurs
/// here), so only the tag is split on '='.
std::expected<scanned_field, std::string> scan_field(std::string_view raw, std::size_t offset) {
    const std::size_t equals = raw.find('=', offset);
    if (equals == std::string_view::npos) {
        return std::unexpected("fix: field has no '=' separator");
    }
    if (equals == offset) {
        return std::unexpected("fix: field has an empty tag");
    }
    for (std::size_t index = offset; index < equals; ++index) {
        if (!is_digit(raw[index])) {
            return std::unexpected("fix: field tag is not a number");
        }
    }

    int tag_number = 0;
    const char* tag_begin = raw.data() + offset;
    const char* tag_end = raw.data() + equals;
    if (std::from_chars(tag_begin, tag_end, tag_number).ec != std::errc{}) {
        return std::unexpected("fix: field tag does not fit in an int");
    }

    const std::size_t terminator = raw.find(kSoh, equals + 1);
    if (terminator == std::string_view::npos) {
        return std::unexpected("fix: field is not SOH-terminated");
    }
    return scanned_field{
        .tag = tag_number,
        .value = raw.substr(equals + 1, terminator - equals - 1),
        .next = terminator + 1,
    };
}

}  // namespace

std::string format_checksum(std::string_view bytes) {
    unsigned sum = 0;
    for (const char byte : bytes) {
        sum += static_cast<unsigned char>(byte);
    }
    sum %= kCheckSumModulus;

    std::string digits(kCheckSumDigits, '0');
    digits[0] = static_cast<char>('0' + ((sum / 100) % 10));
    digits[1] = static_cast<char>('0' + ((sum / 10) % 10));
    digits[2] = static_cast<char>('0' + (sum % 10));
    return digits;
}

std::string format_utc_timestamp(std::uint64_t realtime_ns) {
    const auto seconds = static_cast<std::time_t>(realtime_ns / kNanosPerSecond);
    const auto millis = static_cast<unsigned>((realtime_ns % kNanosPerSecond) / kNanosPerMilli);

    std::tm utc{};
    ::gmtime_r(&seconds, &utc);

    std::array<char, 24> formatted{};
    const std::size_t length =
        std::strftime(formatted.data(), formatted.size(), "%Y%m%d-%H:%M:%S", &utc);

    std::array<char, 8> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%03u", millis);
    return std::string(formatted.data(), length) + fraction.data();
}

std::string build_message(std::span<const field> fields) {
    // The body is rendered first because BodyLength is defined as its exact
    // byte count: rendering the envelope first would need the length of a
    // field that does not exist yet.
    std::string body;
    std::size_t reserved = 0;
    for (const auto& one : fields) {
        reserved += one.value.size() + 8;
    }
    body.reserve(reserved);

    for (const auto& one : fields) {
        body += std::to_string(one.tag);
        body += '=';
        body += one.value;
        body += kSoh;
    }

    std::string message = begin_string_prefix();
    message += "9=";
    message += std::to_string(body.size());
    message += kSoh;
    message += body;

    // CheckSum covers everything built so far, i.e. up to and including the
    // SOH that terminates the last body field.
    message += checksum_field(format_checksum(message));
    return message;
}

std::string build_message(const session_header& header, std::span<const field> body) {
    std::vector<field> fields;
    fields.reserve(body.size() + 4);
    fields.push_back({.tag = tag::msg_type, .value = std::string(header.msg_type)});
    fields.push_back({.tag = tag::sender_comp_id, .value = std::string(header.sender_comp_id)});
    fields.push_back({.tag = tag::target_comp_id, .value = std::string(header.target_comp_id)});
    fields.push_back({.tag = tag::msg_seq_num, .value = std::to_string(header.msg_seq_num)});
    fields.push_back({.tag = tag::sending_time, .value = std::string(header.sending_time)});
    fields.insert(fields.end(), body.begin(), body.end());
    return build_message(fields);
}

std::span<const parsed_message::field_view> parsed_message::body_fields() const {
    // BeginString and BodyLength lead, CheckSum trails; everything between is
    // the body, starting at MsgType.
    constexpr std::size_t kEnvelopeFields = 3;
    if (fields_.size() <= kEnvelopeFields) {
        return {};
    }
    return std::span<const field_view>(fields_).subspan(2, fields_.size() - kEnvelopeFields);
}

std::optional<std::string_view> parsed_message::get(int tag_number) const {
    return first_value(fields_, tag_number);
}

std::optional<std::int64_t> parsed_message::get_int(int tag_number) const {
    const auto value = get(tag_number);
    if (!value) {
        return std::nullopt;
    }
    return to_int(*value);
}

std::size_t parsed_message::count(int tag_number) const {
    return static_cast<std::size_t>(std::ranges::count_if(
        fields_, [tag_number](const field_view& one) { return one.tag == tag_number; }));
}

std::expected<parsed_message, std::string> parse_message(std::string_view raw) {
    const auto begin_string = scan_field(raw, 0);
    if (!begin_string) {
        return std::unexpected(begin_string.error());
    }
    if (begin_string->tag != tag::begin_string) {
        return std::unexpected("fix: message does not start with BeginString(8)");
    }
    if (begin_string->value != kBeginString) {
        return std::unexpected("fix: unsupported BeginString, expected FIX.4.4");
    }

    const auto body_length = scan_field(raw, begin_string->next);
    if (!body_length) {
        return std::unexpected(body_length.error());
    }
    if (body_length->tag != tag::body_length) {
        return std::unexpected("fix: BodyLength(9) does not follow BeginString(8)");
    }
    std::size_t declared_body_length = 0;
    {
        const char* begin = body_length->value.data();
        const char* end = begin + body_length->value.size();
        const auto result = std::from_chars(begin, end, declared_body_length);
        if (result.ec != std::errc{} || result.ptr != end) {
            return std::unexpected("fix: BodyLength(9) is not a number");
        }
    }

    parsed_message message;
    message.fields_.push_back({.tag = begin_string->tag, .value = begin_string->value});
    message.fields_.push_back({.tag = body_length->tag, .value = body_length->value});

    const std::size_t body_start = body_length->next;
    std::size_t offset = body_start;
    std::size_t check_sum_offset = 0;
    std::string_view check_sum_value;

    while (offset < raw.size()) {
        const auto one = scan_field(raw, offset);
        if (!one) {
            return std::unexpected(one.error());
        }
        if (one->tag == tag::check_sum) {
            check_sum_offset = offset;
            check_sum_value = one->value;
            message.fields_.push_back({.tag = one->tag, .value = one->value});
            offset = one->next;
            break;
        }
        if (message.fields_.size() == 2 && one->tag != tag::msg_type) {
            return std::unexpected("fix: MsgType(35) is not the first body field");
        }
        if (one->tag == tag::msg_type) {
            message.msg_type_ = one->value;
        }
        message.fields_.push_back({.tag = one->tag, .value = one->value});
        offset = one->next;
    }

    if (check_sum_offset == 0) {
        return std::unexpected("fix: message has no CheckSum(10) field");
    }
    if (offset != raw.size()) {
        return std::unexpected("fix: trailing bytes after CheckSum(10)");
    }
    if (message.msg_type_.empty()) {
        return std::unexpected("fix: message has no MsgType(35) field");
    }

    // BodyLength's definition, checked literally: the distance from the first
    // byte after the BodyLength field's SOH to the first byte of "10=".
    const std::size_t actual_body_length = check_sum_offset - body_start;
    if (declared_body_length != actual_body_length) {
        return std::unexpected("fix: BodyLength(9) says " + std::to_string(declared_body_length) +
                               " but the body is " + std::to_string(actual_body_length) + " bytes");
    }

    if (check_sum_value.size() != kCheckSumDigits) {
        return std::unexpected("fix: CheckSum(10) is not exactly three digits");
    }
    const std::string expected = format_checksum(raw.substr(0, check_sum_offset));
    if (check_sum_value != expected) {
        return std::unexpected("fix: CheckSum(10) mismatch, computed " + expected);
    }

    return message;
}

std::optional<std::string_view> group_entry::get(int tag_number) const {
    return first_value(fields_, tag_number);
}

std::optional<std::int64_t> group_entry::get_int(int tag_number) const {
    const auto value = get(tag_number);
    if (!value) {
        return std::nullopt;
    }
    return to_int(*value);
}

std::expected<repeating_group, std::string> read_group(const parsed_message& message, int count_tag,
                                                       std::span<const int> member_tags) {
    const auto fields = message.body_fields();
    const auto count_at = std::ranges::find_if(
        fields,
        [count_tag](const parsed_message::field_view& one) { return one.tag == count_tag; });
    if (count_at == fields.end()) {
        return std::unexpected("fix: message has no NumInGroup field " + std::to_string(count_tag));
    }

    const auto declared = to_int(count_at->value);
    if (!declared || *declared < 0) {
        return std::unexpected("fix: NumInGroup field " + std::to_string(count_tag) +
                               " is not a non-negative integer");
    }

    repeating_group group;
    group.declared_count = static_cast<std::size_t>(*declared);

    // Deliberately no reserve() on the declared count: it arrives from the
    // wire, and sizing an allocation from an unvalidated peer-supplied number
    // is how a one-byte typo becomes an out-of-memory. The vector grows to fit
    // what is genuinely there instead.
    if (group.declared_count == 0) {
        return group;
    }

    const auto is_member = [member_tags](int tag_number) {
        return std::ranges::find(member_tags, tag_number) != member_tags.end();
    };

    const auto group_start = static_cast<std::size_t>(count_at - fields.begin()) + 1;
    if (group_start >= fields.size() || !is_member(fields[group_start].tag)) {
        // A count that promises entries with nothing group-shaped behind it.
        // Nothing to salvage, but truncated() now says so.
        return group;
    }

    // The delimiter is whatever tag opens the first repetition -- 279 on 35=X,
    // 269 on 35=W -- not something the caller has to declare and get wrong.
    const int delimiter = fields[group_start].tag;

    std::vector<std::size_t> entry_starts;
    std::size_t offset = group_start;
    while (offset < fields.size() && is_member(fields[offset].tag)) {
        if (fields[offset].tag == delimiter) {
            if (entry_starts.size() == group.declared_count) {
                // NumInGroup is authoritative: whatever follows is outside the
                // group, even though it is spelled like a member.
                break;
            }
            entry_starts.push_back(offset);
        }
        ++offset;
    }

    group.entries.reserve(entry_starts.size());
    for (std::size_t index = 0; index < entry_starts.size(); ++index) {
        const std::size_t begin = entry_starts[index];
        const std::size_t end =
            (index + 1 < entry_starts.size()) ? entry_starts[index + 1] : offset;
        group.entries.emplace_back(fields.subspan(begin, end - begin));
    }
    return group;
}

framer::framer(std::size_t max_body_length) : max_body_length_(max_body_length) {
    buffer_.reserve(std::min<std::size_t>(max_body_length, 64U * 1024U));
}

void framer::append(std::string_view bytes) {
    compact();
    buffer_.append(bytes);
}

void framer::append(std::span<const std::byte> bytes) {
    append(std::string_view(std::bit_cast<const char*>(bytes.data()), bytes.size()));
}

void framer::compact() {
    if (consumed_ == 0) {
        return;
    }
    buffer_.erase(0, consumed_);
    consumed_ = 0;
}

void framer::fail(std::string reason) {
    if (error_.empty()) {
        error_ = std::move(reason);
    }
}

std::optional<std::string_view> framer::next_message() {
    if (!good()) {
        return std::nullopt;
    }

    std::string_view view(buffer_);
    view.remove_prefix(consumed_);
    if (view.empty()) {
        return std::nullopt;
    }

    // Synchronise on BeginString. Anything else at the head of the stream
    // means this is not a FIX session (or framing has already been lost), and
    // there is deliberately no attempt to hunt forward for the next one.
    const std::string prefix = begin_string_prefix();
    const std::size_t comparable = std::min(view.size(), prefix.size());
    if (view.substr(0, comparable) != std::string_view(prefix).substr(0, comparable)) {
        fail("fix: stream does not start with 8=FIX.4.4");
        return std::nullopt;
    }
    if (view.size() < prefix.size() + 2) {
        return std::nullopt;
    }
    if (view.substr(prefix.size(), 2) != "9=") {
        fail("fix: BodyLength(9) does not follow BeginString(8)");
        return std::nullopt;
    }

    const std::size_t digits_start = prefix.size() + 2;
    std::size_t index = digits_start;
    std::size_t body_length = 0;
    while (index < view.size() && view[index] != kSoh) {
        if (!is_digit(view[index]) || (index - digits_start) >= kMaxBodyLengthDigits) {
            fail("fix: BodyLength(9) is not a number");
            return std::nullopt;
        }
        body_length = (body_length * 10) + static_cast<std::size_t>(view[index] - '0');
        if (body_length > max_body_length_) {
            fail("fix: BodyLength(9) exceeds the maximum accepted message size");
            return std::nullopt;
        }
        ++index;
    }
    if (index == view.size()) {
        // Still waiting for the SOH that ends the BodyLength field. The digit
        // cap above keeps this from being an unbounded wait.
        return std::nullopt;
    }
    if (index == digits_start) {
        fail("fix: BodyLength(9) is empty");
        return std::nullopt;
    }

    const std::size_t body_start = index + 1;
    const std::size_t total = body_start + body_length + kCheckSumFieldSize;
    if (view.size() < total) {
        return std::nullopt;
    }

    // BodyLength claims the body ends here, so a CheckSum field must start
    // exactly here. If it does not, the length was wrong and every byte after
    // it is misaligned.
    if (view.substr(body_start + body_length, 3) != "10=" || view[total - 1] != kSoh) {
        fail("fix: no CheckSum(10) field where BodyLength(9) says the body ends");
        return std::nullopt;
    }

    consumed_ += total;
    ++messages_framed_;
    return view.substr(0, total);
}

}  // namespace feed_handler::fix
