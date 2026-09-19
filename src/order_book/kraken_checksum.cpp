#include "order_book/kraken_checksum.h"

#include <optional>
#include <string>
#include <vector>

#include "order_book/types.h"

namespace order_book {
namespace {

// Standard CRC-32 (the zlib/ISO-HDLC/PKZIP variant: reflected, polynomial
// 0xEDB88320, init/final XOR 0xFFFFFFFF). Bit-by-bit rather than
// table-based -- this is the golden book, correctness over speed.
std::uint32_t Crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return ~crc;
}

// Appends the top kKrakenChecksumLevels distinct price levels' orders (in the order
// BookView already carries them: price-priority, then arrival order
// within a level) to out, as concatenated decimal price+quantity tokens.
void AppendTopLevels(std::string& out, const std::vector<BookEntry>& entries) {
    int levels_seen = 0;
    std::optional<Price> current_price;
    for (const BookEntry& entry : entries) {
        if (!current_price.has_value() || entry.price != *current_price) {
            ++levels_seen;
            if (levels_seen > kKrakenChecksumLevels) {
                break;
            }
            current_price = entry.price;
        }
        out += std::to_string(entry.price.Value());
        out += std::to_string(entry.quantity.Lots());
    }
}

}  // namespace

std::uint32_t KrakenL3Checksum(const BookView& view) {
    std::string buffer;
    AppendTopLevels(buffer, view.Entries(Side::kAsk));
    AppendTopLevels(buffer, view.Entries(Side::kBid));
    return Crc32(buffer);
}

}  // namespace order_book
