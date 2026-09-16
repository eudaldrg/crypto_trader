#include "feed_handler/journal_format.h"

#include <zlib.h>

#include <algorithm>

namespace feed_handler::journal {

std::uint32_t Crc32Update(std::uint32_t seed, std::span<const std::byte> data) {
    uLong running = seed;
    // zlib's length argument is uInt; chunk so an oversized span can still be
    // hashed correctly rather than silently truncated by the cast.
    constexpr std::size_t kChunk = 1U << 30U;
    std::span<const std::byte> rest = data;
    while (!rest.empty()) {
        const std::size_t take = std::min(kChunk, rest.size());
        running =
            ::crc32(running, std::bit_cast<const Bytef*>(rest.data()), static_cast<uInt>(take));
        rest = rest.subspan(take);
    }
    return static_cast<std::uint32_t>(running);
}

}  // namespace feed_handler::journal
