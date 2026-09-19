#pragma once

#include <concepts>
#include <optional>

#include "order_book/book_view.h"
#include "order_book/types.h"

// decisions/0006, "Polymorphism mechanism" and "Listener dispatch": every
// axis of the order book (listener shape, matching capability) is resolved
// through concepts, not virtual dispatch or CRTP. A type satisfies a
// concept structurally -- a plain struct with the right method signatures
// is enough, which is also this project's test-mocking story (no mocking
// framework, no shared base class).
namespace order_book {

// Mandatory baseline every ListenerT must satisfy, for every granularity.
// A missing or wrong-signature on_top_of_book_changed is a hard compile
// error here, not a silently-skipped call -- see decisions/0006's
// mandatory-vs-optional hook split.
template <typename T>
concept CoreListener = requires(T& listener, Side side, std::optional<BookEntry> best) {
    { listener.OnTopOfBookChanged(side, best) } -> std::same_as<void>;
};

// Optional hook set: per-level notifications (L2 and L3). Detected via
// if-constexpr(requires{...}) in the engine (T4), not enforced on
// ListenerT itself -- a listener that only implements CoreListener is
// still a valid listener.
template <typename T>
concept LevelListener = requires(T& listener, Side side, Price price, Quantity quantity) {
    { listener.OnLevelChanged(side, price, quantity) } -> std::same_as<void>;
    { listener.OnLevelDeleted(side, price) } -> std::same_as<void>;
};

// Optional hook: the shared, feed-agnostic integrity-anomaly notification
// used by both L2 and L3 (Gap, ChecksumMismatch, CrossedBook, UnknownOrder,
// UnknownLevel) -- see decisions/0006's error taxonomy. Deliberately not
// Kraken- or Deribit-specific.
template <typename T>
concept IntegrityListener = requires(T& listener, IntegrityIssue issue) {
    { listener.OnIntegrityCheckFailed(issue) } -> std::same_as<void>;
};

// Optional hook set: individual-order notifications, L3 only.
template <typename T>
concept OrderListener =
    requires(T& listener, OrderId order_id, Side side, Price price, Quantity quantity) {
        { listener.OnOrderAdded(order_id, side, price, quantity) } -> std::same_as<void>;
        { listener.OnOrderModified(order_id, quantity) } -> std::same_as<void>;
        { listener.OnOrderDeleted(order_id) } -> std::same_as<void>;
    };

// The read/query surface a GranularityPolicy must expose (decisions/0006,
// "Query surface and the BookView oracle") -- independent of ListenerT.
// Readiness is deliberately not part of this concept: the engine owns the
// Uninitialized/Ready/Desynced state machine itself (decisions/0006,
// "Engine/policy contract"), so a policy has nothing to report there.
template <typename T>
concept BookPolicy = requires(const T& policy, Side side) {
    { policy.Best(side) } -> std::same_as<std::optional<BookEntry>>;
};

// The minimal shape any MatchingPolicy must expose (decisions/0006,
// "Matching axis"). NoMatchingPolicy is the only implementation for now;
// a real matching engine is a follow-on plan, but the seam is
// concept-constrained from the start so a future implementation -- or a
// GoogleTest fake -- just has to satisfy this structurally, with no base
// class. Named MatchingPolicyConcept, not MatchingPolicy, so it doesn't
// collide with the template parameter of the same conceptual role in
// engine.h.
template <typename T>
concept MatchingPolicyConcept = requires(T& policy) {
    { policy.MatchingEnabled() } -> std::same_as<bool>;
};

}  // namespace order_book
