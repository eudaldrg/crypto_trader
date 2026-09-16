// Runnable Kraken level3 capture: connect, subscribe, journal every inbound
// message until SIGINT. The first vertical slice of decisions/0004 -- no order
// book, no strategy, no order entry.
//
// Credentials come from the environment (KRAKEN_API_KEY / KRAKEN_API_SECRET)
// and are never read from a file by this process, never logged and never
// journaled.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "feed_handler/capture_session.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/kraken/kraken_ws_client.h"
#include "feed_handler/logging.h"

namespace {

constexpr const char* kJournalDirectory = "journal";
// Local runtime state, alongside ./journal and gitignored for the same reason:
// it belongs to this machine's process, not to the repository.
constexpr const char* kStateDirectory = "state";
constexpr const char* kNonceStateFile = "kraken-nonce.state";
constexpr const char* kSymbol = "BTC/USD";
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

/// Fetches instrument reference data and logs the tick size. Not blocking for
/// capture: the journal holds raw bytes, and nothing downstream of it exists
/// yet to need a tick size (decisions/0004 keeps reference data out of the
/// journal entirely).
void LogInstrumentReference(feed_handler::kraken::RestClient& rest) {
    const auto loaded = rest.LoadAssetPairs();
    if (!loaded) {
        feed_handler::LogWarn("AssetPairs lookup failed, continuing without reference data: " +
                              loaded.error());
        return;
    }
    const feed_handler::kraken::AssetPair* pair = rest.FindAssetPair(kSymbol);
    if (pair == nullptr) {
        // The XBT/BTC trap: REST reports wsname "XBT/USD" for the instrument
        // WS v2 calls "BTC/USD" (exchanges/kraken.md).
        feed_handler::LogWarn(std::string("no AssetPairs entry for ") + kSymbol +
                              ", continuing without reference data");
        return;
    }
    feed_handler::LogInfo("loaded " + std::to_string(*loaded) + " asset pairs; " + kSymbol +
                          " is " + pair->rest_name + " tick_size=" + pair->tick_size +
                          " price_decimals=" + std::to_string(pair->price_decimals) +
                          " lot_decimals=" + std::to_string(pair->qty_decimals));
}

}  // namespace

int main() {
    feed_handler::kraken::Credentials creds{
        .api_key = EnvOrEmpty("KRAKEN_API_KEY"),
        .api_secret_b64 = EnvOrEmpty("KRAKEN_API_SECRET"),
    };
    if (creds.api_key.empty() || creds.api_secret_b64.empty()) {
        feed_handler::LogError(
            "KRAKEN_API_KEY and KRAKEN_API_SECRET must be set in the "
            "environment (level3 is an authenticated channel)");
        return 1;
    }

    std::signal(SIGINT, RequestShutdown);
    std::signal(SIGTERM, RequestShutdown);

    // One RestClient for the whole process: it owns the nonce high-water mark
    // that keeps signed calls strictly increasing across reconnects
    // (exchanges/kraken.md), so it must never be rebuilt per connection. The
    // state file extends that guarantee across restarts -- best effort, and
    // never a reason not to start (kraken_signing.h).
    feed_handler::kraken::RestClient rest(
        std::string(feed_handler::kraken::RestClient::kDefaultBaseUrl),
        std::filesystem::path(kStateDirectory) / kNonceStateFile);
    LogInstrumentReference(rest);

    feed_handler::CaptureSession session({
        .directory = kJournalDirectory,
        .exchange = "kraken",
    });

    feed_handler::kraken::WsClient client(rest, creds, session, {.symbol = kSymbol});
    client.Start();

    while (g_shutdown_requested == 0 && !client.Fatal()) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    const bool fatal = client.Fatal();
    feed_handler::LogInfo(fatal ? "capture failed, shutting down" : "shutdown requested");
    client.Stop();
    session.Close();
    feed_handler::LogInfo("captured " + std::to_string(client.MessagesReceived()) +
                          " messages across " + std::to_string(session.Incarnation()) +
                          " incarnation(s), " + std::to_string(session.TotalRecordsWritten()) +
                          " journal records, " + std::to_string(client.ForcedReconnects()) +
                          " watchdog-forced reconnect(s)");
    return fatal ? 1 : 0;
}
