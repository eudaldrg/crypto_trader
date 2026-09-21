// Copies the start of a v1 capture journal into a small one, for use as a test
// fixture, and prints only counts.
//
// Usage: journal_slice [--max-incrementals N] <input> <output>
//
// The slice is a prefix of the input: the file header, the connect record, and
// every wire message up to and including the Nth Deribit FIX 35=X, so it holds
// the first snapshot (35=W) and a bounded run of incrementals. Records are
// copied byte for byte, capture_sequence and monotonic_ns included, so the
// slice is a valid v1 journal and the input's timing survives. What it changes:
//
//   - every 35=A Logon is DROPPED. Deribit's inbound Logon reply carries
//     RawData (96) and Password (554), which are credential-shaped
//     (exchanges/deribit.md). The record is skipped, so the slice has a gap in
//     capture_sequence there, which the reader and the adapter do not check.
//   - if any record that is kept still holds a 96= or 554= field, nothing is
//     written and the tool fails: a slice is either clean or absent.
//
// It never prints, logs or returns a payload byte: only counts, and error text
// that names a record's position. The counts of 35=W and 35=X are a raw scan of
// the record bytes here, independent of the FIX parser and of the book adapter,
// which is what a replay test asserts the adapter's own counts against.
//
// Exit status: 0 on success, 1 on a usage error, an unreadable or corrupt
// input, or a slice that would not be clean.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "feed_handler/journal_format.h"

namespace {

namespace journal = feed_handler::journal;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;

// The first snapshot is about 5 KB and an incremental about 300 bytes, so this
// keeps the slice under 300 KB.
constexpr std::uint64_t kDefaultMaxIncrementals = 800;

// The SOH-delimited tags looked at. The leading SOH is part of each pattern so
// "35=" cannot match inside another tag such as 135=.
constexpr std::string_view kMsgTypePrefix =
    "\x01"
    "35=";
constexpr std::string_view kLogonMarker =
    "\x01"
    "35=A\x01";
constexpr std::string_view kRawDataTag =
    "\x01"
    "96=";
constexpr std::string_view kPasswordTag =
    "\x01"
    "554=";

struct Arguments {
    std::uint64_t max_incrementals = kDefaultMaxIncrementals;
    std::filesystem::path input;
    std::filesystem::path output;
};

struct Counts {
    std::uint64_t records_read = 0;
    std::uint64_t records_kept = 0;
    std::uint64_t connect_records = 0;
    std::uint64_t snapshots = 0;       // 35=W kept
    std::uint64_t incrementals = 0;    // 35=X kept
    std::uint64_t other_kept = 0;      // any other 35= kept (35=0 heartbeats, ...)
    std::uint64_t logons_dropped = 0;  // 35=A dropped
    std::uint64_t bytes_kept = 0;      // the whole output file
    // A raw scan of every byte of the finished slice. All three must be zero for
    // it to be written.
    std::uint64_t slice_logons = 0;
    std::uint64_t slice_raw_data = 0;
    std::uint64_t slice_password = 0;
};

std::uint64_t CountOccurrences(std::string_view text, std::string_view pattern) {
    std::uint64_t count = 0;
    for (std::size_t pos = text.find(pattern); pos != std::string_view::npos;
         pos = text.find(pattern, pos + pattern.size())) {
        ++count;
    }
    return count;
}

// The value of the first 35= in `text`, up to the next SOH; empty when there is
// none.
std::string_view MsgType(std::string_view text) {
    const std::size_t pos = text.find(kMsgTypePrefix);
    if (pos == std::string_view::npos) {
        return {};
    }
    const std::size_t begin = pos + kMsgTypePrefix.size();
    const std::size_t end = text.find('\x01', begin);
    return end == std::string_view::npos ? std::string_view{} : text.substr(begin, end - begin);
}

std::uint64_t ParseCount(const std::string& flag, const std::string& text) {
    const std::string message = "invalid value for " + flag + ": " + text;
    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error(message);
    }
    if (consumed != text.size() || value == 0) {
        throw std::runtime_error(message);
    }
    return value;
}

Arguments ParseArguments(const std::vector<std::string>& args) {
    Arguments parsed;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--max-incrementals") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for --max-incrementals");
            }
            parsed.max_incrementals = ParseCount(args[i], args[i + 1]);
            ++i;
        } else if (args[i].starts_with("--")) {
            throw std::runtime_error("unknown flag: " + args[i]);
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) {
        throw std::runtime_error("expected an input journal and an output path");
    }
    parsed.input = positional[0];
    parsed.output = positional[1];
    if (parsed.input == parsed.output) {
        throw std::runtime_error("the output must not be the input");
    }
    return parsed;
}

std::span<const std::byte> AsBytes(const std::string& text) {
    return std::as_bytes(std::span<const char>(text));
}

// `size` bytes, or nullopt when the file ends first.
std::optional<std::string> ReadExactly(std::ifstream& file, std::size_t size) {
    std::string bytes(size, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(size));
    if (std::cmp_not_equal(file.gcount(), size)) {
        return std::nullopt;
    }
    return bytes;
}

struct Record {
    std::string header;
    std::string payload;
    std::string trailer;
    std::uint8_t type = 0;
};

// The next record, whole and with its CRC checked, or nullopt at a clean end of
// file. Throws on anything else; `index` only names the record in the message.
std::optional<Record> ReadRecord(std::ifstream& file, std::uint64_t index) {
    if (file.peek() == std::ifstream::traits_type::eof()) {
        return std::nullopt;
    }
    const std::string where = "record " + std::to_string(index) + ": ";
    Record record;
    auto header = ReadExactly(file, journal::kRecordHeaderSize);
    if (!header) {
        throw std::runtime_error(where + "truncated header");
    }
    record.header = std::move(*header);
    record.type = std::to_integer<std::uint8_t>(AsBytes(record.header)[0]);
    const auto length = journal::LoadLe<std::uint32_t>(AsBytes(record.header).subspan(4));
    if (!journal::IsKnownRecordType(record.type) || length > journal::kMaxPayloadBytes) {
        throw std::runtime_error(where + "unusable header");
    }
    auto payload = ReadExactly(file, length);
    auto trailer = ReadExactly(file, journal::kRecordTrailerSize);
    if (!payload || !trailer) {
        throw std::runtime_error(where + "truncated");
    }
    record.payload = std::move(*payload);
    record.trailer = std::move(*trailer);
    std::uint32_t crc = journal::Crc32Update(journal::kCrc32Seed, AsBytes(record.header));
    crc = journal::Crc32Update(crc, AsBytes(record.payload));
    if (crc != journal::LoadLe<std::uint32_t>(AsBytes(record.trailer))) {
        throw std::runtime_error(where + "checksum mismatch");
    }
    return record;
}

// Counts `record` and says whether it goes into the slice: everything but a
// Logon does.
bool Tally(const Record& record, Counts& counts) {
    ++counts.records_read;
    if (record.type == static_cast<std::uint8_t>(journal::RecordType::kConnect)) {
        ++counts.connect_records;
    } else {
        const std::string_view msg_type = MsgType(record.payload);
        if (msg_type == "A") {
            ++counts.logons_dropped;
            return false;
        }
        if (msg_type == "W") {
            ++counts.snapshots;
        } else if (msg_type == "X") {
            ++counts.incrementals;
        } else {
            ++counts.other_kept;
        }
    }
    ++counts.records_kept;
    return true;
}

// Builds the slice in memory (a slice is small) and returns it, or throws with
// the position of what was wrong. Nothing is written until it is known clean.
std::string BuildSlice(const Arguments& arguments, Counts& counts) {
    std::ifstream file(arguments.input, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + arguments.input.string());
    }
    auto header = ReadExactly(file, journal::kFileHeaderSize);
    if (!header ||
        !std::ranges::equal(AsBytes(*header).first(journal::kMagic.size()), journal::kMagic)) {
        throw std::runtime_error("not a v1 journal: bad or short file header");
    }

    std::string slice = std::move(*header);
    while (counts.incrementals < arguments.max_incrementals) {
        const std::optional<Record> record = ReadRecord(file, counts.records_read);
        if (!record) {
            break;
        }
        if (Tally(*record, counts)) {
            slice += record->header;
            slice += record->payload;
            slice += record->trailer;
        }
    }

    // Independent of the per-record classification above: the whole slice, as
    // bytes, is searched for what must not be in it.
    counts.slice_logons = CountOccurrences(slice, kLogonMarker);
    counts.slice_raw_data = CountOccurrences(slice, kRawDataTag);
    counts.slice_password = CountOccurrences(slice, kPasswordTag);
    if (counts.slice_logons != 0 || counts.slice_raw_data != 0 || counts.slice_password != 0) {
        throw std::runtime_error(
            "the slice still holds a Logon or a credential-shaped field, nothing written");
    }
    if (counts.snapshots == 0) {
        throw std::runtime_error("the slice holds no 35=W snapshot, nothing written");
    }
    counts.bytes_kept = slice.size();
    return slice;
}

void PrintCounts(const Counts& counts) {
    std::cout << "records read: " << counts.records_read << "\n"
              << "records kept: " << counts.records_kept << " (connect " << counts.connect_records
              << ", 35=W " << counts.snapshots << ", 35=X " << counts.incrementals
              << ", other 35= " << counts.other_kept << ")\n"
              << "35=A dropped: " << counts.logons_dropped << "\n"
              << "in the slice: 35=A " << counts.slice_logons << ", 96= " << counts.slice_raw_data
              << ", 554= " << counts.slice_password << "\n"
              << "slice size: " << counts.bytes_kept << " bytes\n";
}

int Run(const Arguments& arguments) {
    Counts counts;
    const std::string slice = BuildSlice(arguments, counts);
    std::ofstream out(arguments.output, std::ios::binary | std::ios::trunc);
    out.write(slice.data(), static_cast<std::streamsize>(slice.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + arguments.output.string());
    }
    PrintCounts(counts);
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> all_args(argv, static_cast<std::size_t>(argc));
        const auto args = all_args.subspan(1);
        return Run(ParseArguments(std::vector<std::string>(args.begin(), args.end())));
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n"
                  << "usage: journal_slice [--max-incrementals N] <input> <output>\n";
        return kExitError;
    }
}
