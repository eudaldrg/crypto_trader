#include "order_book/instrument_scale.h"

#include <gtest/gtest.h>

#include "order_book/types.h"

namespace order_book {
namespace {

// Kraken BTC/USD as captured: 1 price decimal, 8 quantity decimals.
constexpr int kPriceDecimals = 1;
constexpr int kQuantityDecimals = 8;

TEST(InstrumentScale, ConvertsToIntegerUnitsAtTheConfiguredDecimals) {
    const InstrumentScale scale(kPriceDecimals, kQuantityDecimals);

    EXPECT_EQ(scale.ToPrice(44939.5), Price(449395));
    EXPECT_EQ(scale.ToQuantity(4.52308393), Quantity(452308393));
}

TEST(InstrumentScale, ConvertsBackToDecimals) {
    const InstrumentScale scale(kPriceDecimals, kQuantityDecimals);

    EXPECT_DOUBLE_EQ(scale.FromPrice(Price(449395)), 44939.5);
    EXPECT_DOUBLE_EQ(scale.FromQuantity(Quantity(452308393)), 4.52308393);
}

// value * 10^decimals is rarely an exact integer in binary floating point;
// the conversion has to round to nearest rather than truncate.
TEST(InstrumentScale, RoundsToNearestInsteadOfTruncating) {
    const InstrumentScale scale(kPriceDecimals, kQuantityDecimals);

    EXPECT_EQ(scale.ToQuantity(0.29), Quantity(29000000));
    EXPECT_EQ(scale.ToPrice(0.3), Price(3));
}

TEST(InstrumentScale, ZeroDecimalsIsTheIdentity) {
    const InstrumentScale whole_units(0, 0);

    EXPECT_EQ(whole_units.ToPrice(42.0), Price(42));
    EXPECT_EQ(whole_units.ToQuantity(7.0), Quantity(7));
}

TEST(InstrumentScale, PriceAndQuantityDecimalsAreIndependent) {
    const InstrumentScale scale(2, 0);

    EXPECT_EQ(scale.ToPrice(1.25), Price(125));
    EXPECT_EQ(scale.ToQuantity(3.0), Quantity(3));
}

}  // namespace
}  // namespace order_book
