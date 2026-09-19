#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/instrument_scale.h"
#include "order_book/kraken_l3_policy.h"
#include "order_book/l3_policy.h"
#include "order_book/types.h"

namespace order_book {

// Kraken order ids are strings ("OD6DFH-GL4H2-57Q6ZI"); the book's OrderId is
// an opaque integer. How the mapping is done is the caller's choice.
using OrderIdMapper = std::function<OrderId(const std::string&)>;

// std::hash-based mapping: no state, fine for replay tests and tooling. It is
// not collision-free in general (none across the checked-in capture's distinct
// ids); an adapter that must never conflate two orders should intern instead.
inline OrderId HashOrderId(const std::string& raw) {
    return OrderId(std::hash<std::string>{}(raw));
}

// Parses a journal payload in place, without first copying it into a string.
// With allow_exceptions false, invalid JSON yields a discarded value instead of
// throwing.
inline nlohmann::json ParseWirePayload(std::span<const std::byte> payload,
                                       bool allow_exceptions = true) {
    const auto* first = reinterpret_cast<const char*>(payload.data());
    return nlohmann::json::parse(first, first + payload.size(), nullptr, allow_exceptions);
}

// One parsed Kraken level3 snapshot or update message.
struct KrakenL3Message {
    bool is_snapshot;  // otherwise an update
    ChecksumMeta meta;
    std::vector<KrakenL3Update> orders;  // bids first, then asks; wire order within a side

    [[nodiscard]] L3Snapshot Snapshot() const {
        L3Snapshot snapshot;
        snapshot.orders.reserve(orders.size());
        for (const KrakenL3Update& entry : orders) {
            snapshot.orders.push_back(entry.update);
        }
        return snapshot;
    }
};

namespace kraken_l3_wire_detail {

inline OrderEventKind ToEventKind(const std::string& event) {
    if (event == "add") {
        return OrderEventKind::kAdded;
    }
    if (event == "modify") {
        return OrderEventKind::kModified;
    }
    if (event == "delete") {
        return OrderEventKind::kDeleted;
    }
    throw std::runtime_error("unknown Kraken level3 event: " + event);
}

inline void AppendSide(std::vector<KrakenL3Update>& out, const nlohmann::json& data, Side side,
                       const char* key, const InstrumentScale& scale,
                       const OrderIdMapper& to_order_id) {
    for (const auto& entry : data.at(key)) {
        // Snapshot entries carry no "event": every one is a resting order.
        const OrderEventKind event =
            entry.contains("event") ? ToEventKind(entry.at("event").get_ref<const std::string&>())
                                    : OrderEventKind::kAdded;
        out.push_back(KrakenL3Update{
            L3Update{event, to_order_id(entry.at("order_id").get_ref<const std::string&>()), side,
                     scale.ToPrice(entry.at("limit_price").get<double>()),
                     scale.ToQuantity(entry.at("order_qty").get<double>())},
            entry.at("timestamp").get<std::string>()});
    }
}

}  // namespace kraken_l3_wire_detail

// Returns nullopt for anything that is not a level3 snapshot/update (status,
// subscribe reply, heartbeat, other channels). Throws on a level3 message
// that is malformed. scale is the instrument's price/quantity decimals -- it
// must come from the instrument's reference data, not be assumed.
inline std::optional<KrakenL3Message> ParseKrakenL3Message(const nlohmann::json& message,
                                                           const InstrumentScale& scale,
                                                           const OrderIdMapper& to_order_id) {
    if (!message.contains("channel") ||
        message.at("channel").get_ref<const std::string&>() != "level3") {
        return std::nullopt;
    }
    const std::string& type = message.at("type").get_ref<const std::string&>();
    if (type != "snapshot" && type != "update") {
        return std::nullopt;
    }
    const nlohmann::json& data = message.at("data").at(0);
    KrakenL3Message parsed{
        type == "snapshot", ChecksumMeta{data.at("checksum").get<std::uint32_t>()}, {}};
    kraken_l3_wire_detail::AppendSide(parsed.orders, data, Side::kBid, "bids", scale, to_order_id);
    kraken_l3_wire_detail::AppendSide(parsed.orders, data, Side::kAsk, "asks", scale, to_order_id);
    return parsed;
}

}  // namespace order_book
