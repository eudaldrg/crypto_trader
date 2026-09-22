// Replays a captured Kraken level3 journal through the real Kraken L3 book
// (src/order_book/kraken_l3_policy.h -- the same code the checksum-verified
// tests exercise, not a reimplementation) and dumps one JSON "frame" per
// message: full book state, individual orders. A lightweight step-through
// visualizer (tools/kraken_journal_viewer.py) just renders these frames --
// all order-book logic lives here, not in the viewer, so the picture can
// never drift from what the real book actually does.
//
// A journal is dumped as one instrument: the first symbol it carries. A
// multi-symbol journal has its other symbols skipped, with a warning.
//
// Usage: kraken_journal_dump <journal_path> <output_json_path>
//            [--price-decimals N] [--quantity-decimals N] [--depth N]
//
// The three settings are properties of the captured instrument and
// subscription, not of the book, and cannot be inferred from a journal, so
// they are flags. The defaults match the checked-in capture (Kraken BTC/USD:
// price_decimals=1, lot_decimals=8, subscribed at Kraken's default depth of
// 10); a journal of anything else must pass its own.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "order_book/engine.h"
#include "order_book/instrument_scale.h"
#include "order_book/kraken_checksum.h"
#include "order_book/kraken_l3_policy.h"
#include "order_book/kraken_l3_wire.h"
#include "order_book/l3_policy.h"
#include "order_book/types.h"

namespace {

using nlohmann::json;
using namespace order_book;  // NOLINT(google-build-using-namespace)

struct Settings {
    int price_decimals = 1;
    int quantity_decimals = 8;
    std::size_t depth = 10;
};

struct NullListener {
    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
};

using KrakenBook = OrderBook<KrakenL3Policy, NullListener>;

// Maps the hashed OrderId back to the real order id string. It exists only so
// the dump can show the string; it has no bearing on book correctness. On a
// (vanishingly unlikely) hash collision the first string seen is kept.
class OrderIdNames {
  public:
    OrderId Intern(const std::string& raw) {
        const OrderId id = HashOrderId(raw);
        names_.try_emplace(id.Value(), raw);
        return id;
    }

    [[nodiscard]] std::string NameOf(OrderId id) const {
        const auto found = names_.find(id.Value());
        return found != names_.end() ? found->second : std::to_string(id.Value());
    }

  private:
    std::unordered_map<std::uint64_t, std::string> names_;
};

json DumpSide(const L3Policy& book, Side side, const InstrumentScale& scale,
              const OrderIdNames& names) {
    json levels = json::array();
    for (const L3Policy::LevelDetail& level : book.Levels(side)) {
        json orders = json::array();
        Quantity total{};
        for (const OrderId order_id : level.order_ids) {
            const Quantity quantity = book.OrderQuantity(order_id).value_or(Quantity{});
            total += quantity;
            orders.push_back(
                {{"id", names.NameOf(order_id)}, {"qty", scale.FromQuantity(quantity)}});
        }
        levels.push_back({
            {"price", scale.FromPrice(level.price)},
            {"total_qty", scale.FromQuantity(total)},
            {"orders", orders},
        });
    }
    return levels;
}

json DumpFrame(int index, const std::string& type, const KrakenBook& book, std::uint32_t checksum,
               const InstrumentScale& scale, const OrderIdNames& names) {
    const L3Policy& l3_book = book.Policy().Book();
    return {
        {"index", index},
        {"type", type},
        {"checksum", checksum},
        {"checksum_ok", KrakenL3Checksum(l3_book.GetBookView()) == checksum},
        {"bids", DumpSide(l3_book, Side::kBid, scale, names)},
        {"asks", DumpSide(l3_book, Side::kAsk, scale, names)},
    };
}

int ParseIntFlag(const std::string& name, const std::string& value) {
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid value for " + name + ": " + value);
    }
}

Settings ParseSettings(const std::vector<std::string>& flags) {
    Settings settings;
    for (std::size_t i = 0; i < flags.size(); i += 2) {
        if (i + 1 >= flags.size()) {
            throw std::runtime_error("missing value for " + flags[i]);
        }
        const int value = ParseIntFlag(flags[i], flags[i + 1]);
        if (flags[i] == "--price-decimals") {
            settings.price_decimals = value;
        } else if (flags[i] == "--quantity-decimals") {
            settings.quantity_decimals = value;
        } else if (flags[i] == "--depth") {
            if (value <= 0) {
                throw std::runtime_error("--depth must be positive");
            }
            settings.depth = static_cast<std::size_t>(value);
        } else {
            throw std::runtime_error("unknown flag: " + flags[i]);
        }
    }
    return settings;
}

// Body of main() split out so exceptions (a malformed message, a bad flag) are
// caught in one place at the real entry point, rather than propagating out of
// main() uncaught.
int RunDump(const std::filesystem::path& journal_path, const std::filesystem::path& output_path,
            const Settings& settings) {
    auto reader = feed_handler::JournalReader::Open(journal_path);
    if (!reader.has_value()) {
        std::cerr << "failed to open journal: " << reader.error() << "\n";
        return 1;
    }

    const InstrumentScale scale(settings.price_decimals, settings.quantity_decimals);
    OrderIdNames names;
    const OrderIdMapper to_order_id = [&names](const std::string& raw) {
        return names.Intern(raw);
    };

    NullListener listener;
    KrakenBook book(listener, KrakenL3Policy(settings.depth));
    json frames = json::array();
    int frame_index = 0;
    std::string dumped_symbol;
    int skipped_other_symbol = 0;

    while (const std::optional<feed_handler::JournalRecord> record = reader->Next()) {
        if (record->type != feed_handler::journal::RecordType::kWireMessage) {
            continue;
        }
        const json wire = ParseWirePayload(record->payload, /*allow_exceptions=*/false);
        if (wire.is_discarded()) {
            continue;
        }
        for (const KrakenL3Message& message : ParseKrakenL3Messages(wire, scale, to_order_id)) {
            // One book, so one symbol: the first one seen. Entries for any
            // other symbol are skipped rather than applied to the wrong book.
            if (dumped_symbol.empty()) {
                dumped_symbol = message.symbol;
            }
            if (message.symbol != dumped_symbol) {
                ++skipped_other_symbol;
                continue;
            }

            if (message.is_snapshot) {
                book.ApplySnapshot(message.Snapshot(), message.meta);
            } else {
                if (!book.IsReady()) {
                    continue;
                }
                book.ApplyBatch(std::span<const KrakenL3Update>(message.orders), message.meta);
            }
            frames.push_back(DumpFrame(frame_index, message.is_snapshot ? "snapshot" : "update",
                                       book, message.meta.checksum, scale, names));
            ++frame_index;
        }
    }

    if (skipped_other_symbol > 0) {
        std::cerr << "warning: skipped " << skipped_other_symbol
                  << " message(s) for symbols other than " << dumped_symbol << "\n";
    }
    if (reader->StoppedEarly()) {
        std::cerr << "warning: journal reader stopped early: " << reader->StopReason() << "\n";
    }

    std::ofstream out(output_path);
    out << frames;
    out.flush();
    if (!out) {
        std::cerr << "failed to write " << output_path << "\n";
        return 1;
    }
    std::cerr << "wrote " << frames.size() << " frames to " << output_path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: kraken_journal_dump <journal_path> <output_json_path>"
                     " [--price-decimals N] [--quantity-decimals N] [--depth N]\n";
        return 1;
    }
    try {
        const std::vector<std::string> flags(argv + 3, argv + argc);
        return RunDump(argv[1], argv[2], ParseSettings(flags));
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
