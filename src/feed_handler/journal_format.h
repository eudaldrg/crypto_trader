// On-disk layout of the v1 capture journal, shared by the writer and the
// reader so the byte offsets have exactly one definition.
// See decisions/0004-feed-handler-architecture.md, "Journal format v1".
//
// One append-only file per (exchange, connection-incarnation). All integers
// are little-endian on the wire regardless of host byte order.
//
// File header (64 bytes):
//    offset size field
//     0      8   magic            "CTJOURNL"
//     8      2   format_version   uint16, currently 1
//    10      2   header_size      uint16, currently 64
//    12      4   flags            uint32, reserved, 0
//    16      8   realtime_ns      uint64, CLOCK_REALTIME at file creation
//    24      8   monotonic_ns     uint64, CLOCK_MONOTONIC at the same instant
//    32     16   exchange         ASCII, NUL-padded ("kraken")
//    48      8   incarnation      uint64, connection-incarnation ordinal
//    56      4   reserved         uint32, 0
//    60      4   crc32            CRC-32 of bytes [0, 60)
//
// The realtime/monotonic pair is the anchor that makes the per-record
// monotonic timestamps convertible to wall clock for this incarnation;
// monotonic alone cannot be correlated across a restart or against
// exchange-side timestamps.
//
// Record (24-byte header + payload + 4-byte trailer):
//    offset size field
//     0      1   RecordType      uint8, see RecordType below
//     1      1   flags            uint8, reserved, 0
//     2      2   reserved         uint16, 0
//     4      4   payload_length   uint32
//     8      8   capture_sequence uint64
//    16      8   monotonic_ns     uint64, CLOCK_MONOTONIC at capture
//    24      N   payload          raw wire bytes, verbatim
//    24+N    4   crc32            CRC-32 of bytes [0, 24+N)
//
// The trailing CRC covers the record header as well as the payload, so it
// detects both a corrupted body and a length field mangled by a crash
// mid-write. A reader treats the first record that fails to read in full or
// fails its CRC as end-of-valid-data, not as an error to propagate.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace feed_handler::journal {

inline constexpr std::array<std::byte, 8> kMagic = {
    std::byte{'C'}, std::byte{'T'}, std::byte{'J'}, std::byte{'O'},
    std::byte{'U'}, std::byte{'R'}, std::byte{'N'}, std::byte{'L'},
};

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kFileHeaderSize = 64;
inline constexpr std::size_t kExchangeFieldSize = 16;
inline constexpr std::size_t kRecordHeaderSize = 24;
inline constexpr std::size_t kRecordTrailerSize = 4;

/// Caps a single record so a corrupt length field can't make a reader try to
/// allocate an absurd buffer. Two orders of magnitude above any plausible
/// Kraken level3 message.
inline constexpr std::uint32_t kMaxPayloadBytes = 64U * 1024U * 1024U;

enum class RecordType : std::uint8_t {
    /// A raw inbound wire message, verbatim. Outbound requests are never
    /// journaled: Kraken's `subscribe` payload carries the live WS token
    /// in-body (see exchanges/kraken.md).
    kWireMessage = 1,

    /// "New connection incarnation, fresh snapshot follows." An explicit
    /// record so nothing reading the journal has to infer a reconnect from
    /// message content. Its payload is a free-form UTF-8 reason string
    /// (possibly empty), not wire data.
    kConnectionIncarnation = 2,
};

/// True for record types this format version knows how to interpret.
constexpr bool IsKnownRecordType(std::uint8_t raw) {
    return raw == static_cast<std::uint8_t>(RecordType::kWireMessage) ||
           raw == static_cast<std::uint8_t>(RecordType::kConnectionIncarnation);
}

/// Writes `value` little-endian into the first sizeof(T) bytes of `out`.
/// Shift-based so it is correct on any host byte order.
template <typename T>
constexpr void StoreLe(std::span<std::byte> out, T value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        out[index] = static_cast<std::byte>((bits >> (index * 8U)) & 0xFFU);
    }
}

/// Reads a little-endian T from the first sizeof(T) bytes of `in`.
template <typename T>
constexpr T LoadLe(std::span<const std::byte> in) {
    std::uint64_t bits = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bits |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[index]))
                << (index * 8U);
    }
    return static_cast<T>(bits);
}

/// Continues a running CRC-32 (zlib polynomial) over `data`. zlib is already a
/// transitive dependency via IXWebSocket's USE_ZLIB, so this adds nothing new.
/// Seed a fresh computation with kCrc32Seed.
std::uint32_t Crc32Update(std::uint32_t seed, std::span<const std::byte> data);

/// zlib's canonical CRC-32 starting value (crc32(0, Z_NULL, 0)).
inline constexpr std::uint32_t kCrc32Seed = 0;

/// CRC-32 over a single contiguous buffer.
inline std::uint32_t Crc32Of(std::span<const std::byte> data) {
    return Crc32Update(kCrc32Seed, data);
}

}  // namespace feed_handler::journal
