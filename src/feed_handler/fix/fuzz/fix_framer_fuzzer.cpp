// libFuzzer harness for the FIX framer/parser pipeline (decisions/0007).
//
// Drives the same two calls a real session driver makes: feed raw bytes to a
// fresh Framer, pull out whatever whole messages it can find, and run
// ParseMessage() on each one. No oracle on parsed field values -- the point
// is crash/UB detection under ASan, not behavioral verification (that's what
// fix_message_test.cpp is for).
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "feed_handler/fix/fix_message.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    feed_handler::fix::Framer framer;
    framer.Append(std::span<const std::byte>(std::bit_cast<const std::byte*>(data), size));

    while (framer.Good()) {
        const auto message = framer.NextMessage();
        if (!message.has_value()) {
            break;
        }
        // Result deliberately ignored: a malformed message returning an error
        // is expected and uninteresting, a crash is what the fuzzer is for.
        (void)feed_handler::fix::ParseMessage(*message);
    }

    return 0;
}
