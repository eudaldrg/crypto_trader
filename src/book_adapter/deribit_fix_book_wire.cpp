#include "book_adapter/deribit_fix_book_wire.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "feed_handler/fix/fix_message.h"
#include "order_book/instrument_scale.h"
#include "order_book/l2_policy.h"
#include "order_book/types.h"

namespace book_adapter {

namespace {

namespace fix = feed_handler::fix;

// MDEntryType(269) values Deribit sends for a book side.
constexpr std::int64_t kEntryTypeBid = 0;
constexpr std::int64_t kEntryTypeOffer = 1;

// MDUpdateAction(279) values.
constexpr std::int64_t kActionNew = 0;
constexpr std::int64_t kActionChange = 1;
constexpr std::int64_t kActionDelete = 2;

// The two entry shapes, as exchanges/deribit.md describes them. The delimiter,
// the first tag of a repetition, is 269 on a snapshot and 279 on an incremental.
constexpr std::array kSnapshotEntryTags = {fix::tag::kMdEntryType, fix::tag::kMdEntryPx,
                                           fix::tag::kMdEntrySize, fix::tag::kMdEntryDate};
constexpr std::array kIncrementalEntryTags = {fix::tag::kMdUpdateAction, fix::tag::kMdEntryType,
                                              fix::tag::kMdEntryPx, fix::tag::kMdEntrySize,
                                              fix::tag::kMdEntryDate};

using Failure = std::unexpected<DeribitFixParseError>;

Failure Fail(std::string what, std::string_view symbol = {}) {
    return Failure(DeribitFixParseError{.what = std::move(what), .symbol = std::string(symbol)});
}

// A finite decimal such as "64000.5", the whole of `text`. from_chars rather
// than strtod: no locale, no allocation, and no accepting a trailing junk byte.
std::optional<double> ParseDecimal(std::string_view text) {
    double value = 0;
    const char* const begin = std::to_address(text.begin());
    const char* const end = std::to_address(text.end());
    const auto [stop, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || stop != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::string EntryText(const char* problem, std::size_t index) {
    return std::string(problem) + " in entry " + std::to_string(index);
}

// One entry's side, price and size. The size of a Delete means nothing, so only
// a New or a Change needs one.
std::expected<order_book::L2Update, std::string> ParseEntry(
    const fix::GroupEntry& entry, std::size_t index, order_book::L2Operation operation,
    const order_book::InstrumentScale& scale) {
    const std::optional<std::int64_t> entry_type = entry.GetInt(fix::tag::kMdEntryType);
    if (!entry_type) {
        return std::unexpected(EntryText("no usable MDEntryType(269)", index));
    }
    order_book::Side side{};
    if (*entry_type == kEntryTypeBid) {
        side = order_book::Side::kBid;
    } else if (*entry_type == kEntryTypeOffer) {
        side = order_book::Side::kAsk;
    } else {
        return std::unexpected(EntryText("MDEntryType(269) is neither bid nor offer", index));
    }

    const std::optional<std::string_view> price_text = entry.Get(fix::tag::kMdEntryPx);
    const std::optional<double> price = price_text ? ParseDecimal(*price_text) : std::nullopt;
    if (!price) {
        return std::unexpected(EntryText("no usable MDEntryPx(270)", index));
    }

    double size = 0;
    const std::optional<std::string_view> size_text = entry.Get(fix::tag::kMdEntrySize);
    if (size_text) {
        const std::optional<double> parsed = ParseDecimal(*size_text);
        if (!parsed || *parsed < 0) {
            return std::unexpected(EntryText("no usable MDEntrySize(271)", index));
        }
        size = *parsed;
    } else if (operation != order_book::L2Operation::kDelete) {
        return std::unexpected(EntryText("no MDEntrySize(271)", index));
    }

    return order_book::L2Update{.side = side,
                                .price = scale.ToPrice(*price),
                                .quantity = scale.ToQuantity(size),
                                .operation = operation};
}

std::expected<order_book::L2Operation, std::string> ParseAction(const fix::GroupEntry& entry,
                                                                std::size_t index) {
    const std::optional<std::int64_t> action = entry.GetInt(fix::tag::kMdUpdateAction);
    if (!action) {
        return std::unexpected(EntryText("no usable MDUpdateAction(279)", index));
    }
    switch (*action) {
        case kActionNew:
            return order_book::L2Operation::kNew;
        case kActionChange:
            return order_book::L2Operation::kChange;
        case kActionDelete:
            return order_book::L2Operation::kDelete;
        default:
            return std::unexpected(
                EntryText("MDUpdateAction(279) is not New, Change or Delete", index));
    }
}

// Reads the NoMDEntries group with `member_tags`, refusing one that ends short.
std::expected<fix::RepeatingGroup, std::string> ReadEntries(const fix::ParsedMessage& message,
                                                            std::span<const int> member_tags) {
    auto group = fix::ReadGroup(message, fix::tag::kNoMdEntries, member_tags);
    if (!group) {
        return std::unexpected(group.error());
    }
    if (group->Truncated()) {
        return std::unexpected("NoMDEntries(268) declares " +
                               std::to_string(group->declared_count) + " entries but " +
                               std::to_string(group->size()) + " are present");
    }
    return group;
}

}  // namespace

std::expected<DeribitFixBookMessage, DeribitFixParseError> ParseDeribitFixMessage(
    std::span<const std::byte> payload, const order_book::InstrumentScale& scale) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view raw(reinterpret_cast<const char*>(payload.data()), payload.size());
    const auto parsed = fix::ParseMessage(raw);
    if (!parsed) {
        return Fail(parsed.error());
    }

    const std::string_view type = parsed->MsgType();
    const bool is_snapshot = type == fix::msg_type::kMarketDataSnapshotFullRefresh;
    if (!is_snapshot && type != fix::msg_type::kMarketDataIncrementalRefresh) {
        return DeribitFixBookMessage{};
    }

    const std::optional<std::string_view> symbol = parsed->Get(fix::tag::kSymbol);
    if (!symbol || symbol->empty()) {
        return Fail("market data message without a Symbol(55)");
    }

    DeribitFixBookMessage message;
    message.symbol = std::string(*symbol);
    const auto group =
        ReadEntries(*parsed, is_snapshot ? std::span<const int>(kSnapshotEntryTags)
                                         : std::span<const int>(kIncrementalEntryTags));
    if (!group) {
        return Fail(group.error(), *symbol);
    }

    if (is_snapshot) {
        message.kind = FixBookMessageKind::kSnapshot;
        message.snapshot.levels.reserve(group->size());
    } else {
        message.kind = FixBookMessageKind::kUpdate;
        message.updates.reserve(group->size());
    }
    std::size_t index = 0;
    for (const fix::GroupEntry& entry : *group) {
        std::expected<order_book::L2Operation, std::string> operation =
            order_book::L2Operation::kNew;
        if (!is_snapshot) {
            operation = ParseAction(entry, index);
        }
        if (!operation) {
            return Fail(operation.error(), *symbol);
        }
        const auto update = ParseEntry(entry, index, *operation, scale);
        if (!update) {
            return Fail(update.error(), *symbol);
        }
        (is_snapshot ? message.snapshot.levels : message.updates).push_back(*update);
        ++index;
    }
    return message;
}

}  // namespace book_adapter
