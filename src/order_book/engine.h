#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <utility>

#include "order_book/concepts.h"
#include "order_book/no_matching_policy.h"
#include "order_book/types.h"

namespace order_book {

// The shared engine every granularity plugs into (decisions/0006, "Shared
// engine and the engine/policy contract"). It owns listener storage, the
// Uninitialized/Ready/Desynced readiness state machine, the
// Apply()/ApplySnapshot() entry points, and the (currently unused)
// MatchingPolicy seam. GranularityPolicy::Apply(...) returns a
// policy-defined ChangeSet that Notify() below translates into listener
// calls via if-constexpr(requires{...}) -- so granularity-specific
// behavior lives in the ChangeSet shape and the policy, not as branching
// here on which concrete policy is active.
//
// Notify() and ApplyReadinessTransition() detect whichever of these a
// given ChangeSet actually carries: {bid,ask}_top_of_book_changed (L1,
// L2), level_changes (L2, L3), integrity_issue (L2, L3). If a future
// granularity needs a shape these don't cover, extend both rather than
// forcing it into what's here.
template <class GranularityPolicy, typename ListenerT, class MatchingPolicy = NoMatchingPolicy>
    requires BookPolicy<GranularityPolicy> && CoreListener<ListenerT> &&
             MatchingPolicyConcept<MatchingPolicy>
class OrderBook {
  public:
    // The policy is taken by value so one that needs construction-time
    // settings (e.g. L3Policy's depth) can be passed in already configured.
    explicit OrderBook(ListenerT& listener, GranularityPolicy policy = GranularityPolicy{})
        : listener_(listener), policy_(std::move(policy)) {}

    [[nodiscard]] Readiness GetReadiness() const {
        return readiness_;
    }
    [[nodiscard]] bool IsReady() const {
        return readiness_ == Readiness::kReady;
    }
    [[nodiscard]] std::optional<BookEntry> Best(Side side) const {
        return policy_.Best(side);
    }

    [[nodiscard]] GranularityPolicy& Policy() {
        return policy_;
    }
    [[nodiscard]] const GranularityPolicy& Policy() const {
        return policy_;
    }
    [[nodiscard]] bool MatchingEnabled() const {
        return matching_policy_.MatchingEnabled();
    }

    // A snapshot fully replaces the policy's state, so the policy always
    // reports both sides changed regardless of whether the fresh value
    // happens to equal what came before -- a listener resuming interest
    // after a reconnect/gap needs the current truth, not a diff against
    // state it may never have seen. MessageMeta is variadic (zero or one
    // argument) so granularities with nothing to validate at snapshot time
    // (L1, L2) can omit it while L3 passes a ChecksumMeta, mirroring
    // ApplyBatch's meta parameter -- the returned ChangeSet is what lets a
    // snapshot's own checksum (Kraken) or malformed state (a crossed book)
    // get reported and reflected in readiness_, exactly like an ordinary
    // batch, instead of being applied unconditionally and unchecked.
    //
    // Constrained on the policy accepting exactly this snapshot and meta, so
    // handing a sequenced policy no meta (or an unsequenced one a meta) is
    // rejected at the call site instead of deep inside the policy.
    template <typename Snapshot, typename... MessageMeta>
        requires requires(GranularityPolicy& policy, const Snapshot& snapshot,
                          const MessageMeta&... meta) { policy.ApplySnapshot(snapshot, meta...); }
    void ApplySnapshot(const Snapshot& snapshot, const MessageMeta&... meta) {
        const auto change_set = policy_.ApplySnapshot(snapshot, meta...);
        readiness_ = Readiness::kReady;
        ApplyReadinessTransition(change_set);
        Notify(change_set);
    }

    // Applying an update on a not-Ready book is a defined no-op
    // (decisions/0006, "Snapshot and readiness"), not undefined behavior --
    // there is nothing valid to apply before a snapshot has established a
    // baseline.
    template <typename Update>
        requires requires(GranularityPolicy& policy, const Update& update) { policy.Apply(update); }
    void Apply(const Update& update) {
        if (readiness_ != Readiness::kReady) {
            return;
        }
        const auto change_set = policy_.Apply(update);
        ApplyReadinessTransition(change_set);
        Notify(change_set);
    }

    // Message/batch boundary (decisions/0006): a batch's sequencing or
    // checksum is meaningful only once the whole message has been
    // applied, so this validates/applies as one unit rather than
    // update-by-update. Same not-Ready guard as Apply(): a batch on a
    // Desynced book is a defined no-op until a fresh ApplySnapshot()
    // recovers it.
    // MessageMeta is variadic (zero or one argument), like ApplySnapshot's:
    // L2 passes its ChangeIdMeta, an exchange-specific L3 policy passes its
    // checksum, and the generic L3 book has nothing to validate and passes none.
    template <typename Update, typename... MessageMeta>
        requires requires(GranularityPolicy& policy, std::span<const Update> updates,
                          const MessageMeta&... meta) { policy.ApplyBatch(updates, meta...); }
    void ApplyBatch(std::span<const Update> updates, const MessageMeta&... meta) {
        if (readiness_ != Readiness::kReady) {
            return;
        }
        const auto change_set = policy_.ApplyBatch(updates, meta...);
        ApplyReadinessTransition(change_set);
        Notify(change_set);
    }

  private:
    template <typename ChangeSet>
    void ApplyReadinessTransition(const ChangeSet& change_set) {
        if constexpr (requires {
                          {
                              change_set.integrity_issue
                          } -> std::convertible_to<std::optional<IntegrityIssue>>;
                      }) {
            if (change_set.integrity_issue.has_value()) {
                readiness_ = Readiness::kDesynced;
            }
        }
    }

    template <typename ChangeSet>
    void Notify(const ChangeSet& change_set) {
        if constexpr (requires {
                          { change_set.bid_top_of_book_changed } -> std::convertible_to<bool>;
                          { change_set.ask_top_of_book_changed } -> std::convertible_to<bool>;
                      }) {
            if (change_set.bid_top_of_book_changed) {
                listener_.OnTopOfBookChanged(Side::kBid, policy_.Best(Side::kBid));
            }
            if (change_set.ask_top_of_book_changed) {
                listener_.OnTopOfBookChanged(Side::kAsk, policy_.Best(Side::kAsk));
            }
        }
        if constexpr (LevelListener<ListenerT> && requires { change_set.level_changes.begin(); }) {
            for (const auto& change : change_set.level_changes) {
                if (change.quantity.has_value()) {
                    listener_.OnLevelChanged(change.side, change.price, *change.quantity);
                } else {
                    listener_.OnLevelDeleted(change.side, change.price);
                }
            }
        }
        if constexpr (OrderListener<ListenerT> && requires { change_set.order_events.begin(); }) {
            for (const auto& event : change_set.order_events) {
                switch (event.kind) {
                    case OrderEventKind::kAdded:
                        listener_.OnOrderAdded(event.order_id, event.side, event.price,
                                               event.quantity);
                        break;
                    case OrderEventKind::kModified:
                        listener_.OnOrderModified(event.order_id, event.quantity);
                        break;
                    case OrderEventKind::kDeleted:
                        listener_.OnOrderDeleted(event.order_id);
                        break;
                }
            }
        }
        if constexpr (IntegrityListener<ListenerT> && requires {
                          {
                              change_set.integrity_issue
                          } -> std::convertible_to<std::optional<IntegrityIssue>>;
                      }) {
            if (change_set.integrity_issue.has_value()) {
                listener_.OnIntegrityCheckFailed(*change_set.integrity_issue);
            }
        }
    }

    ListenerT& listener_;
    GranularityPolicy policy_;
    MatchingPolicy matching_policy_{};
    Readiness readiness_ = Readiness::kUninitialized;
};

}  // namespace order_book
