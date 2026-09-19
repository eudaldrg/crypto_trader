// libFuzzer harness for Framer + ParseMessage (decisions/0007); crash/UB only, no oracle.
#include <cstddef>
#include <cstdint>
#include <span>

#include "feed_handler/fix/fix_message.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    feed_handler::fix::Framer framer;
    framer.Append(std::as_bytes(std::span(data, size)));

    while (framer.Good()) {
        const auto message = framer.NextMessage();
        if (!message.has_value()) {
            break;
        }
        // A malformed message returning an error is expected; a crash is the finding.
        (void)feed_handler::fix::ParseMessage(*message);
    }

    return 0;
}
