// Replays captured journals through the real BookAdapter, inline, and prints
// what happened: message and update counts, integrity issues by kind, and parse
// and apply wall time separately. This is the profiling harness for the book
// code; run it on a long capture rather than a unit-test fixture.
//
// Usage: journal_replay [--price-decimals N] [--quantity-decimals N] [--depth N]
//                       [--no-timing] <journal> [<journal> ...]
//
// The journals are one connection's files, in order (see journal_replay.h). The
// three numeric settings are properties of the captured instrument and
// subscription, which a journal cannot record, so they are flags. The defaults
// match the checked-in Kraken capture (BTC/USD: price decimals 1, quantity
// decimals 8, subscribed at Kraken's default depth 10); a journal of anything
// else must pass its own.
//
// Exit status: 0 on a complete replay, 1 on a usage error or a journal that
// cannot be replayed at all, 2 when a journal stopped early (the report is still
// printed, but its counts are not the whole capture).
#include "book_adapter/journal_replay.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/instrument_scale.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitStoppedEarly = 2;

constexpr int kDefaultPriceDecimals = 1;
constexpr int kDefaultQuantityDecimals = 8;
// The scale asserts on more than this many decimals (instrument_scale.h).
constexpr int kMaxDecimals = 15;

struct Arguments {
    int price_decimals = kDefaultPriceDecimals;
    int quantity_decimals = kDefaultQuantityDecimals;
    std::size_t depth = book_adapter::kDefaultKrakenDepth;
    bool measure_timing = true;
    std::vector<std::filesystem::path> journals;
};

int ParseNumber(const std::string& flag, const std::string& text) {
    const std::string message = "invalid value for " + flag + ": " + text;
    std::size_t consumed = 0;
    int value = 0;
    try {
        value = std::stoi(text, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error(message);
    }
    if (consumed != text.size()) {
        throw std::runtime_error(message);
    }
    return value;
}

int ParseDecimals(const std::string& flag, const std::string& text) {
    const int value = ParseNumber(flag, text);
    if (value < 0 || value > kMaxDecimals) {
        throw std::runtime_error(flag + " must be between 0 and " + std::to_string(kMaxDecimals));
    }
    return value;
}

Arguments ParseArguments(const std::vector<std::string>& args) {
    Arguments parsed;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--no-timing") {
            parsed.measure_timing = false;
        } else if (arg == "--price-decimals" || arg == "--quantity-decimals" || arg == "--depth") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for " + arg);
            }
            const std::string& value = args[++i];
            if (arg == "--price-decimals") {
                parsed.price_decimals = ParseDecimals(arg, value);
            } else if (arg == "--quantity-decimals") {
                parsed.quantity_decimals = ParseDecimals(arg, value);
            } else {
                const int depth = ParseNumber(arg, value);
                if (depth <= 0) {
                    throw std::runtime_error("--depth must be positive");
                }
                parsed.depth = static_cast<std::size_t>(depth);
            }
        } else if (arg.starts_with("--")) {
            throw std::runtime_error("unknown flag: " + arg);
        } else {
            parsed.journals.emplace_back(arg);
        }
    }
    if (parsed.journals.empty()) {
        throw std::runtime_error("no journal given");
    }
    return parsed;
}

int Run(const Arguments& arguments) {
    book_adapter::JournalReplay replay(book_adapter::ReplayOptions{
        .scale = order_book::InstrumentScale(arguments.price_decimals, arguments.quantity_decimals),
        .kraken_depth = arguments.depth,
        .measure_timing = arguments.measure_timing,
    });
    const auto report = replay.Run(arguments.journals);
    if (!report) {
        std::cerr << "error: " << report.error() << "\n";
        return kExitError;
    }
    book_adapter::PrintReport(std::cout, *report);
    if (report->StoppedEarly()) {
        std::cerr << "warning: a journal stopped early, the counts above are not the whole "
                     "capture\n";
        return kExitStoppedEarly;
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> all_args(argv, static_cast<std::size_t>(argc));
        const auto args = all_args.subspan(1);
        return Run(ParseArguments(std::vector<std::string>(args.begin(), args.end())));
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n"
                  << "usage: journal_replay [--price-decimals N] [--quantity-decimals N] "
                     "[--depth N] [--no-timing] <journal> [<journal> ...]\n";
        return kExitError;
    }
}
