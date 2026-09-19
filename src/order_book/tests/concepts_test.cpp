#include "order_book/concepts.h"

#include <gtest/gtest.h>

#include <optional>

#include "order_book/book_view.h"
#include "order_book/types.h"

namespace order_book {
namespace {

// --- CoreListener ---

struct MinimalListener {
    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
};
static_assert(CoreListener<MinimalListener>);

struct EmptyType {};
static_assert(!CoreListener<EmptyType>);

// Near-miss: right name, wrong signature (missing the best-entry
// parameter). Must be rejected, not silently treated as "hook absent".
struct WrongArityListener {
    void OnTopOfBookChanged(Side) {}
};
static_assert(!CoreListener<WrongArityListener>);

// Near-miss: right name and arity, wrong return type.
struct WrongReturnListener {
    int OnTopOfBookChanged(Side, std::optional<BookEntry>) {
        return 0;
    }
};
static_assert(!CoreListener<WrongReturnListener>);

// --- LevelListener ---

struct LevelOnlyListener {
    void OnLevelChanged(Side, Price, Quantity) {}
    void OnLevelDeleted(Side, Price) {}
};
static_assert(LevelListener<LevelOnlyListener>);
static_assert(!LevelListener<MinimalListener>);

struct PartialLevelListener {
    void OnLevelChanged(Side, Price, Quantity) {}
    // on_level_deleted missing entirely.
};
static_assert(!LevelListener<PartialLevelListener>);

// --- IntegrityListener ---

struct IntegrityOnlyListener {
    void OnIntegrityCheckFailed(IntegrityIssue) {}
};
static_assert(IntegrityListener<IntegrityOnlyListener>);
static_assert(!IntegrityListener<MinimalListener>);

// --- OrderListener ---

struct OrderOnlyListener {
    void OnOrderAdded(OrderId, Side, Price, Quantity) {}
    void OnOrderModified(OrderId, Quantity) {}
    void OnOrderDeleted(OrderId) {}
};
static_assert(OrderListener<OrderOnlyListener>);
static_assert(!OrderListener<MinimalListener>);

struct WrongOrderModifiedListener {
    void OnOrderAdded(OrderId, Side, Price, Quantity) {}
    // Near-miss: on_order_modified missing its Quantity parameter.
    void OnOrderModified(OrderId) {}
    void OnOrderDeleted(OrderId) {}
};
static_assert(!OrderListener<WrongOrderModifiedListener>);

// A listener can freely combine tiers -- there's no shared base class
// forcing an all-or-nothing implementation.
struct FullListener {
    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
    void OnLevelChanged(Side, Price, Quantity) {}
    void OnLevelDeleted(Side, Price) {}
    void OnIntegrityCheckFailed(IntegrityIssue) {}
    void OnOrderAdded(OrderId, Side, Price, Quantity) {}
    void OnOrderModified(OrderId, Quantity) {}
    void OnOrderDeleted(OrderId) {}
};
static_assert(CoreListener<FullListener>);
static_assert(LevelListener<FullListener>);
static_assert(IntegrityListener<FullListener>);
static_assert(OrderListener<FullListener>);

// --- BookPolicy ---

struct MinimalPolicy {
    std::optional<BookEntry> Best(Side) const {
        return std::nullopt;
    }
};
static_assert(BookPolicy<MinimalPolicy>);
static_assert(!BookPolicy<EmptyType>);

// Near-miss: a non-const Best() does not satisfy BookPolicy -- the engine
// only ever calls Best() through a const GranularityPolicy&.
struct NonConstBestPolicy {
    std::optional<BookEntry> Best(Side) {
        return std::nullopt;
    }
};
static_assert(!BookPolicy<NonConstBestPolicy>);

struct WrongReturnPolicy {
    // Near-miss: returns BookEntry instead of std::optional<BookEntry>.
    BookEntry Best(Side) const {
        return {};
    }
};
static_assert(!BookPolicy<WrongReturnPolicy>);

TEST(Concepts, StaticAssertionsCompile) {
    SUCCEED();
}

}  // namespace
}  // namespace order_book
