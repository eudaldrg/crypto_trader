#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "order_book/l3_policy.h"
#include "order_book/tests/l3_test_util.h"
#include "order_book/types.h"

namespace order_book {

// Kraken's own worked example for the level3 checksum algorithm
// (https://docs.kraken.com/api/docs/guides/spot-ws-l3-v2/, fetched and
// independently re-verified against the documented asks/bids strings and
// their standard CRC-32 before being transcribed here as integer
// ticks/lots -- decisions/0006 explicitly calls out not trusting a
// self-derived vector, so this one is Kraken's, not invented). It is an
// independent oracle, unlike a checksum computed by our own code.
//
// Price is in ticks of 0.1, quantity in lots of 0.00000001 -- e.g. price
// 449395 is 44939.5, quantity 452308393 is 4.52308393. Both scales were
// derived directly from the documented example. It has exactly 10 price
// levels per side, which is the checksum's own window.
inline constexpr std::uint32_t kKrakenDocumentedChecksum = 1063832831U;
inline constexpr std::size_t kKrakenDocumentedLevelsPerSide = 10;

inline std::vector<L3Update> KrakenDocumentedExampleOrders() {
    return {
        // Asks, price ascending (low to high).
        l3_test::Add(1, Side::kAsk, 449395, 452308393),
        l3_test::Add(2, Side::kAsk, 449395, 111261),
        l3_test::Add(3, Side::kAsk, 449395, 100000),
        l3_test::Add(4, Side::kAsk, 449395, 1000000),
        l3_test::Add(5, Side::kAsk, 449500, 10334926),
        l3_test::Add(6, Side::kAsk, 449530, 64537),
        l3_test::Add(7, Side::kAsk, 449550, 250000),
        l3_test::Add(8, Side::kAsk, 449596, 35630000),
        l3_test::Add(9, Side::kAsk, 449596, 35630000),
        l3_test::Add(10, Side::kAsk, 449601, 338072),
        l3_test::Add(11, Side::kAsk, 449602, 88967575),
        l3_test::Add(12, Side::kAsk, 449670, 314392283),
        l3_test::Add(13, Side::kAsk, 449785, 6778960),
        l3_test::Add(14, Side::kAsk, 449792, 35630000),
        // Bids, price descending (high to low).
        l3_test::Add(15, Side::kBid, 449394, 88968699),
        l3_test::Add(16, Side::kBid, 449394, 45210000),
        l3_test::Add(17, Side::kBid, 449394, 10000000),
        l3_test::Add(18, Side::kBid, 449394, 14296323),
        l3_test::Add(19, Side::kBid, 449394, 25000000),
        l3_test::Add(20, Side::kBid, 449394, 10292988),
        l3_test::Add(21, Side::kBid, 449394, 33880000),
        l3_test::Add(22, Side::kBid, 449394, 128140860),
        l3_test::Add(23, Side::kBid, 449371, 3346877),
        l3_test::Add(24, Side::kBid, 449347, 35630000),
        l3_test::Add(25, Side::kBid, 449302, 22734299),
        l3_test::Add(26, Side::kBid, 449302, 1000000),
        l3_test::Add(27, Side::kBid, 449302, 5550000),
        l3_test::Add(28, Side::kBid, 449302, 70000000),
        l3_test::Add(29, Side::kBid, 449302, 15000000),
        l3_test::Add(30, Side::kBid, 449280, 105240),
        l3_test::Add(31, Side::kBid, 449196, 33870000),
        l3_test::Add(32, Side::kBid, 449195, 7610000),
        l3_test::Add(33, Side::kBid, 449120, 35630000),
        l3_test::Add(34, Side::kBid, 449097, 6690000),
        l3_test::Add(35, Side::kBid, 449019, 88982),
    };
}

}  // namespace order_book
