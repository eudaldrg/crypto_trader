// Runnable Deribit FIX capture: log on to the testnet, request BTC-PERPETUAL
// market data, and journal every inbound FIX message until SIGINT. The second
// exchange backend of decisions/0004 -- no order book, no strategy, no order
// entry, and no repeating-group parsing of the 35=W/35=X entry lists.
//
// Credentials come from the environment (DERIBIT_TESTNET_CLIENT_ID /
// DERIBIT_TESTNET_CLIENT_SECRET) and are never read from a file by this
// process, never logged and never journaled -- the journal holds inbound bytes
// only, and journal_writer has no outbound path at all.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include "feed_handler/capture_session.h"
#include "feed_handler/deribit/deribit_fix_client.h"
#include "feed_handler/deribit/deribit_fix_session.h"
#include "feed_handler/logging.h"

namespace {

constexpr const char* kJournalDirectory = "journal";
constexpr const char* kSymbol = "BTC-PERPETUAL";
constexpr std::chrono::milliseconds kShutdownPollInterval{100};

// A signal handler may only touch a volatile sig_atomic_t, so this cannot be
// wrapped in anything nicer.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void request_shutdown(int /*signal*/) {
    g_shutdown_requested = 1;
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

}  // namespace

int main() {
    feed_handler::deribit::session_config session{
        .client_id = env_or_empty("DERIBIT_TESTNET_CLIENT_ID"),
        .client_secret = env_or_empty("DERIBIT_TESTNET_CLIENT_SECRET"),
    };
    if (session.client_id.empty() || session.client_secret.empty()) {
        feed_handler::log_error(
            "DERIBIT_TESTNET_CLIENT_ID and DERIBIT_TESTNET_CLIENT_SECRET must "
            "be set in the environment (the FIX Logon password is derived from "
            "the secret)");
        return 1;
    }

    std::signal(SIGINT, request_shutdown);
    std::signal(SIGTERM, request_shutdown);

    feed_handler::capture_session capture({
        .directory = kJournalDirectory,
        .exchange = "deribit",
    });

    feed_handler::deribit::fix_client client(std::move(session), capture, {.symbol = kSymbol});
    client.start();

    while (g_shutdown_requested == 0 && !client.fatal()) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    const bool fatal = client.fatal();
    feed_handler::log_info(fatal ? "capture failed, shutting down" : "shutdown requested");
    // stop() joins the connection thread, which sends the Logout and closes the
    // socket on its way out -- the socket is never touched from this thread.
    client.stop();
    capture.close();
    feed_handler::log_info("captured " + std::to_string(client.messages_received()) +
                           " messages (" + std::to_string(client.snapshots_received()) +
                           " snapshot(s), " + std::to_string(client.incrementals_received()) +
                           " incremental(s)) across " + std::to_string(capture.incarnation()) +
                           " incarnation(s), " + std::to_string(capture.total_records_written()) +
                           " journal records, " + std::to_string(client.connection_attempts()) +
                           " connection attempt(s), " + std::to_string(client.forced_reconnects()) +
                           " watchdog-forced reconnect(s)");
    return fatal ? 1 : 0;
}
