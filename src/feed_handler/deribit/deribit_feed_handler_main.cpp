// Runnable Deribit FIX capture: log on to the testnet, request BTC-PERPETUAL
// market data, and journal every inbound FIX message until SIGINT. The second
// exchange backend of decisions/0004 -- no order book, no strategy, no order
// entry, and no repeating-group parsing of the 35=W/35=X entry lists.
//
// Credentials come from the environment (DERIBIT_TESTNET_CLIENT_ID /
// DERIBIT_TESTNET_CLIENT_SECRET) and are never read from a file by this
// process, never logged and never journaled -- the journal holds inbound bytes
// only, and JournalWriter has no outbound path at all.
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

extern "C" void RequestShutdown(int /*signal*/) {
    g_shutdown_requested = 1;
}

std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

}  // namespace

int main() {
    feed_handler::deribit::SessionConfig session{
        .client_id = EnvOrEmpty("DERIBIT_TESTNET_CLIENT_ID"),
        .client_secret = EnvOrEmpty("DERIBIT_TESTNET_CLIENT_SECRET"),
    };
    if (session.client_id.empty() || session.client_secret.empty()) {
        feed_handler::LogError(
            "DERIBIT_TESTNET_CLIENT_ID and DERIBIT_TESTNET_CLIENT_SECRET must "
            "be set in the environment (the FIX Logon password is derived from "
            "the secret)");
        return 1;
    }

    std::signal(SIGINT, RequestShutdown);
    std::signal(SIGTERM, RequestShutdown);

    feed_handler::CaptureSession capture({
        .directory = kJournalDirectory,
        .exchange = "deribit",
    });

    feed_handler::deribit::FixClient client(std::move(session), capture, {.symbol = kSymbol});
    client.Start();

    while (g_shutdown_requested == 0 && !client.Fatal()) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    const bool fatal = client.Fatal();
    feed_handler::LogInfo(fatal ? "capture failed, shutting down" : "shutdown requested");
    // stop() joins the connection thread, which sends the Logout and closes the
    // socket on its way out -- the socket is never touched from this thread.
    client.Stop();
    capture.Close();
    feed_handler::LogInfo("captured " + std::to_string(client.MessagesReceived()) + " messages (" +
                          std::to_string(client.SnapshotsReceived()) + " snapshot(s), " +
                          std::to_string(client.IncrementalsReceived()) +
                          " incremental(s)) across " + std::to_string(capture.Incarnation()) +
                          " incarnation(s), " + std::to_string(capture.TotalRecordsWritten()) +
                          " journal records, " + std::to_string(client.ConnectionAttempts()) +
                          " connection attempt(s), " + std::to_string(client.ForcedReconnects()) +
                          " watchdog-forced reconnect(s)");
    return fatal ? 1 : 0;
}
