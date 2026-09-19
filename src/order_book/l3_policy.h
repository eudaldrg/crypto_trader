#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "order_book/book_view.h"
#include "order_book/types.h"

namespace order_book {

// Exchange-agnostic order-by-order wire shape: add/modify/delete by
// order_id. Modify only ever changes quantity -- a price change is a
// separate delete+add, since it would otherwise lose queue priority
// silently. Anything specific to one exchange's feed (Kraken's checksum,
// its per-order timestamps) lives in that exchange's own policy, not here.
struct L3Update {
    OrderEventKind event;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
};

struct L3Snapshot {
    std::vector<L3Update> orders;  // event is ignored; every entry is a resting order.
};

struct L3ChangeSet {
    bool bid_top_of_book_changed = false;
    bool ask_top_of_book_changed = false;
    std::vector<LevelChange> level_changes;
    std::vector<OrderEvent> order_events;
    std::optional<IntegrityIssue> integrity_issue;
};

// The golden L3 book (decisions/0006): individual orders tracked per price
// level in arrival order (not aggregate-only counts), since a per-order
// checksum such as Kraken's cannot be reconstructed from aggregates.
// order_index_ gives O(log n) lookup by order_id for modify/delete; the
// per-level vectors preserve arrival order for both queue priority and
// level aggregation.
//
// max_depth is the number of price levels per side the book retains. It
// exists for feeds that deliver a depth-limited view and never send an
// explicit delete for a level that falls out of it, so the client has to
// drop those itself (Kraken's level3 channel does exactly this, and the
// depth is whatever the subscription requested). kUnlimitedDepth, the
// default, retains every level.
class L3Policy {
  public:
    static constexpr std::size_t kUnlimitedDepth = std::numeric_limits<std::size_t>::max();

    explicit L3Policy(std::size_t max_depth = kUnlimitedDepth) : max_depth_(max_depth) {
        assert(max_depth_ > 0);
    }

    [[nodiscard]] std::optional<BookEntry> Best(Side side) const {
        return side == Side::kBid ? BestOf(bid_levels_) : BestOf(ask_levels_);
    }

    // Best-to-worst entries of at most max_levels price levels per side (every
    // level by default). A caller that only reads a window of the book, such as
    // Kraken's top-10 checksum, passes it so a deep book is not copied whole.
    [[nodiscard]] BookView GetBookView(std::size_t max_levels = kUnlimitedDepth) const {
        return BookView(EntriesOf(bid_levels_, max_levels), EntriesOf(ask_levels_, max_levels));
    }

    // Full per-level, per-order detail, best-to-worst, arrival order within
    // a level -- for tooling/debugging (e.g. a book visualizer) rather than
    // production logic, which only ever needs BookView. Exposed directly
    // off internal state so it reflects truncation exactly, unlike
    // reconstructing it from listener events (OnOrderAdded/etc. are never
    // fired for orders TruncateToDepth silently drops).
    struct LevelDetail {
        Price price;
        std::vector<OrderId> order_ids;
    };

    [[nodiscard]] std::vector<LevelDetail> Levels(Side side) const {
        return side == Side::kBid ? LevelsOf(bid_levels_) : LevelsOf(ask_levels_);
    }

    [[nodiscard]] std::optional<Quantity> OrderQuantity(OrderId order_id) const {
        const auto it = order_index_.find(order_id);
        return it == order_index_.end() ? std::nullopt
                                        : std::optional<Quantity>(it->second.quantity);
    }

    // Returns a real ChangeSet (rather than discarding one) so a snapshot
    // goes through the same engine-side notify/readiness machinery as an
    // ordinary batch: an OrderListener sees the snapshot's resting orders
    // via OnOrderAdded, and a crossed snapshot is reported.
    L3ChangeSet ApplySnapshot(const L3Snapshot& snapshot) {
        bid_levels_.clear();
        ask_levels_.clear();
        order_index_.clear();
        L3ChangeSet change_set;
        for (const L3Update& order : snapshot.orders) {
            AddOrder(order, change_set);
        }
        TruncateToDepth(bid_levels_);
        TruncateToDepth(ask_levels_);

        change_set.bid_top_of_book_changed = true;
        change_set.ask_top_of_book_changed = true;
        if (IsCrossed(Best(Side::kBid), Best(Side::kAsk))) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kCrossedBook);
        }
        return change_set;
    }

    L3ChangeSet ApplyBatch(std::span<const L3Update> updates) {
        L3ChangeSet change_set;
        const std::optional<BookEntry> old_best_bid = Best(Side::kBid);
        const std::optional<BookEntry> old_best_ask = Best(Side::kAsk);

        for (const L3Update& update : updates) {
            switch (update.event) {
                case OrderEventKind::kAdded:
                    AddOrder(update, change_set);
                    break;
                case OrderEventKind::kModified:
                    ModifyOrder(update, change_set);
                    break;
                case OrderEventKind::kDeleted:
                    DeleteOrder(update, change_set);
                    break;
            }
        }

        // A depth-limited feed sends no delete for levels that fall out of
        // its window, so anything beyond max_depth_ is dropped here. This
        // matters beyond bookkeeping: an order that drops out and later
        // re-enters is re-announced with a fresh Add for the same
        // order_id, which would duplicate against a stale copy this policy
        // kept holding (see AddOrder).
        TruncateToDepth(bid_levels_);
        TruncateToDepth(ask_levels_);

        const std::optional<BookEntry> new_best_bid = Best(Side::kBid);
        const std::optional<BookEntry> new_best_ask = Best(Side::kAsk);
        change_set.bid_top_of_book_changed = old_best_bid != new_best_bid;
        change_set.ask_top_of_book_changed = old_best_ask != new_best_ask;
        if (IsCrossed(new_best_bid, new_best_ask)) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kCrossedBook);
        }
        return change_set;
    }

  private:
    struct OrderRecord {
        Side side;
        Price price;
        Quantity quantity;
    };

    template <typename Map>
    void TruncateToDepth(Map& levels) {
        if (levels.size() <= max_depth_) {
            return;
        }
        auto cutoff = levels.begin();
        std::advance(cutoff, static_cast<std::ptrdiff_t>(max_depth_));
        for (auto it = cutoff; it != levels.end(); ++it) {
            for (const OrderId order_id : it->second) {
                order_index_.erase(order_id);
            }
        }
        levels.erase(cutoff, levels.end());
    }

    [[nodiscard]] Quantity LevelAggregate(const std::vector<OrderId>& order_ids) const {
        Quantity total{};
        for (const OrderId order_id : order_ids) {
            total += order_index_.at(order_id).quantity;
        }
        return total;
    }

    template <typename Map>
    [[nodiscard]] std::optional<BookEntry> BestOf(const Map& levels) const {
        if (levels.empty()) {
            return std::nullopt;
        }
        return BookEntry{levels.begin()->first, LevelAggregate(levels.begin()->second)};
    }

    template <typename Map>
    [[nodiscard]] std::vector<LevelDetail> LevelsOf(const Map& levels) const {
        std::vector<LevelDetail> result;
        for (const auto& [price, order_ids] : levels) {
            result.push_back(LevelDetail{price, order_ids});
        }
        return result;
    }

    template <typename Map>
    [[nodiscard]] std::vector<BookEntry> EntriesOf(const Map& levels,
                                                   std::size_t max_levels) const {
        std::vector<BookEntry> entries;
        std::size_t level_count = 0;
        for (const auto& [price, order_ids] : levels) {
            if (level_count++ == max_levels) {
                break;
            }
            for (const OrderId order_id : order_ids) {
                entries.push_back(BookEntry{price, order_index_.at(order_id).quantity});
            }
        }
        return entries;
    }

    void AddOrder(const L3Update& update, L3ChangeSet& change_set) {
        // A depth-limited L3 feed re-announces an order_id with a fresh Add
        // when it re-enters the tracked window after having dropped out of
        // it -- remove any stale occurrence first so the same order_id is
        // never duplicated within a level, which would corrupt both the
        // aggregate quantity and any per-order checksum.
        const auto existing = order_index_.find(update.order_id);
        if (existing != order_index_.end()) {
            const Side old_side = existing->second.side;
            const Price old_price = existing->second.price;
            order_index_.erase(existing);
            if (old_side == Side::kBid) {
                RemoveOrderFromLevel(bid_levels_, old_side, old_price, update.order_id, change_set);
            } else {
                RemoveOrderFromLevel(ask_levels_, old_side, old_price, update.order_id, change_set);
            }
        }
        order_index_.insert_or_assign(update.order_id,
                                      OrderRecord{update.side, update.price, update.quantity});
        if (update.side == Side::kBid) {
            AddOrderToLevel(bid_levels_, update, change_set);
        } else {
            AddOrderToLevel(ask_levels_, update, change_set);
        }
    }

    template <typename Map>
    void AddOrderToLevel(Map& levels, const L3Update& update, L3ChangeSet& change_set) {
        std::vector<OrderId>& order_ids = levels[update.price];
        order_ids.push_back(update.order_id);
        change_set.order_events.push_back(OrderEvent{OrderEventKind::kAdded, update.order_id,
                                                     update.side, update.price, update.quantity});
        change_set.level_changes.push_back(
            LevelChange{update.side, update.price, LevelAggregate(order_ids)});
    }

    void ModifyOrder(const L3Update& update, L3ChangeSet& change_set) {
        const auto it = order_index_.find(update.order_id);
        if (it == order_index_.end()) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kUnknownOrder);
            return;
        }
        it->second.quantity = update.quantity;
        const Side side = it->second.side;
        const Price price = it->second.price;
        change_set.order_events.push_back(
            OrderEvent{OrderEventKind::kModified, update.order_id, side, price, update.quantity});
        const std::vector<OrderId>& order_ids =
            side == Side::kBid ? bid_levels_.at(price) : ask_levels_.at(price);
        change_set.level_changes.push_back(LevelChange{side, price, LevelAggregate(order_ids)});
    }

    void DeleteOrder(const L3Update& update, L3ChangeSet& change_set) {
        const auto it = order_index_.find(update.order_id);
        if (it == order_index_.end()) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kUnknownOrder);
            return;
        }
        const Side side = it->second.side;
        const Price price = it->second.price;
        order_index_.erase(it);
        if (side == Side::kBid) {
            RemoveOrderFromLevel(bid_levels_, side, price, update.order_id, change_set);
        } else {
            RemoveOrderFromLevel(ask_levels_, side, price, update.order_id, change_set);
        }
    }

    template <typename Map>
    void RemoveOrderFromLevel(Map& levels, Side side, Price price, OrderId order_id,
                              L3ChangeSet& change_set) {
        const auto level_it = levels.find(price);
        assert(level_it != levels.end() && "order_index_ referenced a level that no longer exists");
        std::vector<OrderId>& order_ids = level_it->second;
        order_ids.erase(std::remove(order_ids.begin(), order_ids.end(), order_id), order_ids.end());
        change_set.order_events.push_back(
            OrderEvent{OrderEventKind::kDeleted, order_id, side, price, Quantity{}});
        if (order_ids.empty()) {
            levels.erase(level_it);
            change_set.level_changes.push_back(LevelChange{side, price, std::nullopt});
        } else {
            change_set.level_changes.push_back(LevelChange{side, price, LevelAggregate(order_ids)});
        }
    }

    std::size_t max_depth_;
    LevelMap<Price, std::vector<OrderId>, std::greater<>> bid_levels_;
    LevelMap<Price, std::vector<OrderId>, std::less<>> ask_levels_;
    std::map<OrderId, OrderRecord> order_index_;
};

}  // namespace order_book
