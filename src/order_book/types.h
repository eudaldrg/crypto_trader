#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>

namespace order_book {

enum class Side : std::uint8_t { kBid, kAsk };

// Comparable-only strong id, used where arithmetic is meaningless (prices
// are compared and used as map keys, never summed; order ids are opaque).
template <typename Tag, typename Underlying>
class Ordered {
  public:
    Ordered() = default;
    explicit constexpr Ordered(Underlying value) : value_(value) {}

    [[nodiscard]] constexpr Underlying Value() const {
        return value_;
    }

    friend constexpr auto operator<=>(const Ordered&, const Ordered&) = default;

  private:
    Underlying value_{};
};

using Price = Ordered<struct PriceTag, std::int64_t>;
using OrderId = Ordered<struct OrderIdTag, std::uint64_t>;

// Per-side level map, keyed by price. Cmp = std::greater<> for bids and
// std::less<> for asks means map.begin() is always the best level on
// either side, for any Value type (an aggregate Quantity for L2, a
// per-order std::vector<OrderId> for L3). Kept as an alias
// (decisions/0006, "Level container / comparator") so swapping to an
// Abseil/ankerl btree later is a typedef change, not a rewrite.
template <typename Key, typename Value, typename Cmp>
using LevelMap = std::map<Key, Value, Cmp>;

// Quantities are aggregated (summing orders at a price level), so unlike
// Price/OrderId this carries arithmetic. IsZero() names the wire-shaped
// "zero quantity" encoding used by L1Update/L2Update (see decisions/0006,
// "Wire encoding vs. stored state") without callers spelling out the raw
// comparison at every call site.
class Quantity {
  public:
    Quantity() = default;
    explicit constexpr Quantity(std::int64_t lots) : lots_(lots) {}

    [[nodiscard]] constexpr std::int64_t Lots() const {
        return lots_;
    }
    [[nodiscard]] constexpr bool IsZero() const {
        return lots_ == 0;
    }

    friend constexpr auto operator<=>(const Quantity&, const Quantity&) = default;

    constexpr Quantity& operator+=(Quantity other) {
        lots_ += other.lots_;
        return *this;
    }

  private:
    std::int64_t lots_{};
};

// Feed-integrity anomalies (decisions/0006, "Error taxonomy"): reported
// through one shared listener hook plus a Desynced state transition,
// never silently absorbed.
enum class IntegrityIssue : std::uint8_t {
    kGap,
    kChecksumMismatch,
    kCrossedBook,
    kUnknownOrder,
    kUnknownLevel,
};

// Records an integrity issue unless one is already recorded. The first is the
// most specific: an unknown order followed by a checksum mismatch, say, is one
// problem seen twice.
inline void ReportIssue(std::optional<IntegrityIssue>& slot, IntegrityIssue issue) {
    if (!slot.has_value()) {
        slot = issue;
    }
}

// decisions/0006, "Snapshot and readiness": a book only ever answers
// queries meaningfully in the Ready state. Desynced means a feed-integrity
// anomaly was detected and the book requires a fresh snapshot to recover.
enum class Readiness : std::uint8_t { kUninitialized, kReady, kDesynced };

// One touched price level's new state, shared by L2 and L3's ChangeSet
// (decisions/0006, "Engine/policy contract" -- the engine's Notify()
// detects this shape generically via if-constexpr(requires{...})).
// nullopt quantity means the level was deleted.
struct LevelChange {
    Side side;
    Price price;
    std::optional<Quantity> quantity;
};

// One order lifecycle event, L3 only. side/price are only meaningful for
// Added; Modified/Deleted only need order_id/quantity and order_id
// respectively, but one shape covers all three so the engine can detect
// and dispatch them generically.
enum class OrderEventKind : std::uint8_t { kAdded, kModified, kDeleted };

struct OrderEvent {
    OrderEventKind kind;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
};

}  // namespace order_book
