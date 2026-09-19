#pragma once

#include <cstdint>

#include "order_book/l3_policy.h"
#include "order_book/types.h"

namespace order_book::l3_test {

inline L3Update Add(std::uint64_t order_id, Side side, std::int64_t price, std::int64_t quantity) {
    return L3Update{.event = OrderEventKind::kAdded,
                    .order_id = OrderId(order_id),
                    .side = side,
                    .price = Price(price),
                    .quantity = Quantity(quantity)};
}

// A Modify or Delete is looked up by order_id alone; the book ignores the
// side and price it carries.
inline L3Update Modify(std::uint64_t order_id, std::int64_t quantity) {
    return L3Update{.event = OrderEventKind::kModified,
                    .order_id = OrderId(order_id),
                    .side = Side::kBid,
                    .price = Price{},
                    .quantity = Quantity(quantity)};
}

inline L3Update Delete(std::uint64_t order_id) {
    return L3Update{.event = OrderEventKind::kDeleted,
                    .order_id = OrderId(order_id),
                    .side = Side::kBid,
                    .price = Price{},
                    .quantity = Quantity{}};
}

}  // namespace order_book::l3_test
