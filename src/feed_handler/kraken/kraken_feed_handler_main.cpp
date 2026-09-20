// Runnable Kraken level3 capture: connect, subscribe, journal every inbound
// message until SIGINT. The first vertical slice of decisions/0004 -- no order
// book, no strategy, no order entry.
//
// Which connections to open, to which symbols, is the `--config` TOML file
// (feed_handler/config): every `kraken` entry becomes one socket and one
// journal file. Credentials are named there by environment variable and read
// from the environment here; they are never read from a file by this process,
// never logged and never journaled.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/kraken/kraken_ws_client.h"
#include "feed_handler/logging.h"

namespace {

// Local runtime state, gitignored like ./journal: it belongs to this machine's
// process, not to the repository. The directory is the config's `state_dir`.
constexpr const char* kNonceStateFile = "kraken-nonce.state";
constexpr std::chrono::milliseconds kShutdownPollInterval{100};

// A signal handler may only touch a volatile sig_atomic_t, so this cannot be
// wrapped in anything nicer.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void RequestShutdown(int /*signal*/) {
    g_shutdown_requested = 1;
}

/// Fetches instrument reference data and logs each configured symbol's tick
/// size. Not blocking for capture: the journal holds raw bytes, and nothing
/// downstream of it exists yet to need a tick size (decisions/0004 keeps
/// reference data out of the journal entirely). It doubles as an early typo
/// check: a symbol with no AssetPairs entry is logged, since Kraken would
/// otherwise only reject it on subscribe.
void LogInstrumentReference(feed_handler::kraken::RestClient& rest,
                            const std::vector<feed_handler::config::Connection>& connections) {
    const auto loaded = rest.LoadAssetPairs();
    if (!loaded) {
        feed_handler::LogWarn("AssetPairs lookup failed, continuing without reference data: " +
                              loaded.error());
        return;
    }
    feed_handler::LogInfo("loaded " + std::to_string(*loaded) + " asset pairs");
    for (const auto& connection : connections) {
        for (const std::string& symbol : connection.symbols) {
            const feed_handler::kraken::AssetPair* pair = rest.FindAssetPair(symbol);
            if (pair == nullptr) {
                // The XBT/BTC trap: REST reports wsname "XBT/USD" for the
                // instrument WS v2 calls "BTC/USD" (exchanges/kraken.md).
                feed_handler::LogWarn("[" + connection.id + "] no AssetPairs entry for " + symbol +
                                      ", continuing without reference data");
                continue;
            }
            feed_handler::LogInfo("[" + connection.id + "] " + symbol + " is " + pair->rest_name +
                                  " tick_size=" + pair->tick_size +
                                  " price_decimals=" + std::to_string(pair->price_decimals) +
                                  " lot_decimals=" + std::to_string(pair->qty_decimals));
        }
    }
}

/// One configured connection and everything it owns. The session outlives the
/// client that journals into it, so the order of the members is the order they
/// are torn down in, reversed.
struct Capture {
    std::unique_ptr<feed_handler::CaptureSession> session;
    std::unique_ptr<feed_handler::kraken::WsClient> client;
};

/// Reads the credentials each connection names. Reports every missing variable
/// by NAME before giving up, so one run shows the whole list; values are never
/// printed.
bool ResolveCredentials(const std::vector<feed_handler::config::Connection>& connections,
                        std::vector<feed_handler::kraken::Credentials>& out) {
    bool ok = true;
    for (const auto& connection : connections) {
        feed_handler::kraken::Credentials creds{
            .api_key = feed_handler::config::EnvOrEmpty(connection.api_key_env),
            .api_secret_b64 = feed_handler::config::EnvOrEmpty(connection.api_secret_env),
        };
        if (creds.api_key.empty()) {
            feed_handler::LogError("[" + connection.id + "] environment variable " +
                                   connection.api_key_env +
                                   " is not set (level3 is an authenticated channel)");
            ok = false;
        }
        if (creds.api_secret_b64.empty()) {
            feed_handler::LogError("[" + connection.id + "] environment variable " +
                                   connection.api_secret_env +
                                   " is not set (level3 is an authenticated channel)");
            ok = false;
        }
        out.push_back(std::move(creds));
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config_path = feed_handler::config::ConfigPathFromArgs(argc, argv);
    if (!config_path) {
        feed_handler::LogError(config_path.error());
        return 1;
    }
    const auto config = feed_handler::config::LoadConfigFile(*config_path);
    if (!config) {
        feed_handler::LogError(config.error());
        return 1;
    }
    const auto connections = config->ConnectionsFor(feed_handler::config::Exchange::kKraken);
    if (connections.empty()) {
        feed_handler::LogError("no kraken [[connections]] in " + config_path->string());
        return 1;
    }
    if (std::ranges::any_of(connections, [](const feed_handler::config::Connection& connection) {
            return connection.env != feed_handler::config::Environment::kProd;
        })) {
        // GetWebSocketsToken is a production REST call; there is no Kraken
        // testnet REST endpoint to pair a non-prod websocket with.
        feed_handler::LogError("kraken connections must have env = \"prod\"");
        return 1;
    }

    std::vector<feed_handler::kraken::Credentials> credentials;
    if (!ResolveCredentials(connections, credentials)) {
        return 1;
    }

    std::signal(SIGINT, RequestShutdown);
    std::signal(SIGTERM, RequestShutdown);

    // One RestClient for the whole process, shared by every connection: it owns
    // the nonce high-water mark that keeps signed calls strictly increasing
    // across reconnects (exchanges/kraken.md), so it must never be rebuilt per
    // connection, and it serializes their token fetches. The state file extends
    // that guarantee across restarts -- best effort, and never a reason not to
    // start (kraken_signing.h).
    feed_handler::kraken::RestClient rest(
        std::string(feed_handler::kraken::RestClient::kDefaultBaseUrl),
        config->state_dir / kNonceStateFile);
    LogInstrumentReference(rest, connections);

    std::vector<Capture> captures;
    captures.reserve(connections.size());
    for (std::size_t index = 0; index < connections.size(); ++index) {
        const auto& connection = connections[index];
        Capture capture;
        capture.session = std::make_unique<feed_handler::CaptureSession>(
            feed_handler::CaptureSession::Config{.directory = config->journal_dir,
                                                 .exchange = "kraken"},
            connection.id);
        capture.client = std::make_unique<feed_handler::kraken::WsClient>(
            rest, std::move(credentials[index]), *capture.session,
            feed_handler::kraken::WsClientConfig{.url = connection.endpoint,
                                                 .id = connection.id,
                                                 .symbols = connection.symbols});
        captures.push_back(std::move(capture));
    }
    for (const Capture& capture : captures) {
        capture.client->Start();
    }

    const auto any_fatal = [&captures] {
        return std::ranges::any_of(captures,
                                   [](const Capture& capture) { return capture.client->Fatal(); });
    };
    while (g_shutdown_requested == 0 && !any_fatal()) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    const bool fatal = any_fatal();
    feed_handler::LogInfo(fatal ? "capture failed, shutting down" : "shutdown requested");
    for (const Capture& capture : captures) {
        capture.client->Stop();
        capture.session->Close();
    }
    for (std::size_t index = 0; index < captures.size(); ++index) {
        const Capture& capture = captures[index];
        feed_handler::LogInfo(
            "[" + connections[index].id + "] captured " +
            std::to_string(capture.client->MessagesReceived()) + " messages across " +
            std::to_string(capture.session->Incarnation()) + " incarnation(s), " +
            std::to_string(capture.session->TotalRecordsWritten()) + " journal records, " +
            std::to_string(capture.client->ForcedReconnects()) + " watchdog-forced reconnect(s)");
    }
    return fatal ? 1 : 0;
}
