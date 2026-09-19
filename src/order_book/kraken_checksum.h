#pragma once

#include <cstdint>

#include "order_book/book_view.h"

namespace order_book {

// Kraken's level3 checksum (decisions/0006, "L3 checksum: algorithm and
// placement"): a free function over the golden book's BookView, kept
// outside the generic engine since this is exchange-specific behavior --
// L3Policy calls this itself, the engine and the generic L3 listener
// concept never reference it directly.
//
// Algorithm (verified against Kraken's own worked example, see
// kraken_checksum_test.cpp -- not guessed from the field's existence
// alone, per https://docs.kraken.com/api/docs/guides/spot-ws-l3-v2/):
// for each side, take the top 10 price levels (asks ascending, bids
// descending), and within each level iterate its orders in arrival order
// -- this is what makes the L3 checksum also verify queue priority,
// unlike the L2 book-channel checksum, which only checks aggregate level
// quantities. For each order, concatenate its price then its quantity as
// plain decimal integers (Price/Quantity here are already tick/lot
// integers, which is exactly the "remove the decimal point and strip
// leading zeros" string Kraken's algorithm describes -- no separate
// decimal-places metadata is needed). Concatenate every order's token,
// asks first then bids, and take the standard CRC-32 (the zlib/ISO-HDLC
// variant) of the resulting ASCII string.
std::uint32_t KrakenL3Checksum(const BookView& view);

// How many price levels per side the checksum covers. Fixed by Kraken's
// algorithm and independent of the depth a subscription asked for: a deeper
// subscription still checksums only its top 10 levels.
inline constexpr int kKrakenChecksumLevels = 10;

}  // namespace order_book
