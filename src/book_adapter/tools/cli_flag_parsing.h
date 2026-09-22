// Shared "parse one CLI flag's integer value" helper for this directory's
// tools (journal_replay, journal_slice): std::stoi/std::stoull plus rejecting
// anything but a full parse, so "10x" is an error rather than silently 10.
// Range validation (a flag that must be positive, or capped) is left to the
// caller, same as before this was factored out.
#pragma once

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace book_adapter::tools {

template <typename T>
T ParseFlagValue(const std::string& flag, const std::string& text) {
    static_assert(std::is_same_v<T, int> || std::is_same_v<T, std::uint64_t>,
                  "ParseFlagValue only wraps the std::sto* overloads this directory's tools use");
    const std::string message = "invalid value for " + flag + ": " + text;
    std::size_t consumed = 0;
    T value{};
    try {
        if constexpr (std::is_same_v<T, std::uint64_t>) {
            value = std::stoull(text, &consumed);
        } else {
            value = std::stoi(text, &consumed);
        }
    } catch (const std::exception&) {
        throw std::runtime_error(message);
    }
    if (consumed != text.size()) {
        throw std::runtime_error(message);
    }
    return value;
}

}  // namespace book_adapter::tools
