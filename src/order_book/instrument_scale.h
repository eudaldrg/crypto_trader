#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>

#include "order_book/types.h"

namespace order_book {

// Converts between an exchange's decimal prices/quantities and the book's
// integer Price/Quantity units (ticks and lots). The number of decimal
// places is per-instrument reference data (Kraken's AssetPairs
// pair_decimals/lot_decimals, Deribit's tick size), not a property of the
// book, so callers supply it from configuration and nothing else in this
// subsystem hardcodes a scale.
class InstrumentScale {
  public:
    constexpr InstrumentScale(int price_decimals, int quantity_decimals)
        : price_factor_(Pow10(price_decimals)), quantity_factor_(Pow10(quantity_decimals)) {}

    [[nodiscard]] Price ToPrice(double value) const {
        return Price(static_cast<std::int64_t>(std::llround(value * price_factor_)));
    }
    [[nodiscard]] Quantity ToQuantity(double value) const {
        return Quantity(static_cast<std::int64_t>(std::llround(value * quantity_factor_)));
    }
    [[nodiscard]] double FromPrice(Price price) const {
        return static_cast<double>(price.Value()) / price_factor_;
    }
    [[nodiscard]] double FromQuantity(Quantity quantity) const {
        return static_cast<double>(quantity.Lots()) / quantity_factor_;
    }

  private:
    static constexpr double Pow10(int decimals) {
        assert(decimals >= 0 && decimals <= 15);
        double result = 1;
        for (int i = 0; i < decimals; ++i) {
            result *= 10;
        }
        return result;
    }

    double price_factor_;
    double quantity_factor_;
};

}  // namespace order_book
