// Runnable Deribit FIX capture: log on to the testnet, request the configured
// instruments' market data, and journal every inbound FIX message until SIGINT.
// The second exchange backend of decisions/0004 -- no order book, no strategy,
// no order entry, and no repeating-group parsing of the 35=W/35=X entry lists.
//
// Which connections to open, to which instruments, is the `--config` TOML file
// (feed_handler/config): every `deribit` entry becomes one FIX session and one
// journal file. Credentials are named there by environment variable and read
// from the environment here; they are never read from a file by this process,
// never logged and never journaled -- the journal holds inbound bytes only, and
// JournalWriter has no outbound path at all.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/deribit/deribit_fix_client.h"
#include "feed_handler/deribit/deribit_fix_session.h"
#include "feed_handler/logging.h"

namespace {

constexpr std::chrono::milliseconds kShutdownPollInterval{100};

// A signal handler may only touch a volatile sig_atomic_t, so this cannot be
// wrapped in anything nicer.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void RequestShutdown(int /*signal*/) {
    g_shutdown_requested = 1;
}

/// One configured connection and everything it owns. The session outlives the
/// client that journals into it, so the order of the members is the order they
/// are torn down in, reversed.
struct Capture {
    std::unique_ptr<feed_handler::CaptureSession> session;
    std::unique_ptr<feed_handler::deribit::FixClient> client;
};

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
    const auto connections = config->ConnectionsFor(feed_handler::config::Exchange::kDeribit);
    if (connections.empty()) {
        feed_handler::LogError("no deribit [[connections]] in " + config_path->string());
        return 1;
    }

    // Reads every connection's credentials before opening anything. Missing
    // variables are reported by NAME, all at once; values are never printed.
    std::vector<feed_handler::deribit::SessionConfig> sessions;
    bool credentials_ok = true;
    for (const auto& connection : connections) {
        feed_handler::deribit::SessionConfig session{
            .client_id = feed_handler::config::EnvOrEmpty(connection.api_key_env),
            .client_secret = feed_handler::config::EnvOrEmpty(connection.api_secret_env),
        };
        if (session.client_id.empty() || session.client_secret.empty()) {
            feed_handler::LogError("[" + connection.id + "] environment variables " +
                                   connection.api_key_env + " and " + connection.api_secret_env +
                                   " must both be set (the FIX Logon password is derived from "
                                   "the secret)");
            credentials_ok = false;
        }
        sessions.push_back(std::move(session));
    }
    if (!credentials_ok) {
        return 1;
    }

    std::signal(SIGINT, RequestShutdown);
    std::signal(SIGTERM, RequestShutdown);

    std::vector<Capture> captures;
    captures.reserve(connections.size());
    for (std::size_t index = 0; index < connections.size(); ++index) {
        const auto& connection = connections[index];
        Capture capture;
        capture.session = std::make_unique<feed_handler::CaptureSession>(
            feed_handler::CaptureSession::Config{.directory = config->journal_dir,
                                                 .exchange = "deribit",
                                                 .file_prefix = connection.id});
        capture.client = std::make_unique<feed_handler::deribit::FixClient>(
            std::move(sessions[index]), *capture.session,
            feed_handler::deribit::FixClientConfig{.id = connection.id,
                                                   .host = connection.host_port.host,
                                                   .port = connection.host_port.port,
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
    // Stop() joins the connection thread, which sends the Logout and closes the
    // socket on its way out -- the socket is never touched from this thread.
    for (const Capture& capture : captures) {
        capture.client->Stop();
        capture.session->Close();
    }
    for (std::size_t index = 0; index < captures.size(); ++index) {
        const Capture& capture = captures[index];
        feed_handler::LogInfo(
            "[" + connections[index].id + "] captured " +
            std::to_string(capture.client->MessagesReceived()) + " messages (" +
            std::to_string(capture.client->SnapshotsReceived()) + " snapshot(s), " +
            std::to_string(capture.client->IncrementalsReceived()) + " incremental(s)) across " +
            std::to_string(capture.session->Incarnation()) + " incarnation(s), " +
            std::to_string(capture.session->TotalRecordsWritten()) + " journal records, " +
            std::to_string(capture.client->ConnectionAttempts()) + " connection attempt(s), " +
            std::to_string(capture.client->ForcedReconnects()) + " watchdog-forced reconnect(s)");
    }
    return fatal ? 1 : 0;
}
