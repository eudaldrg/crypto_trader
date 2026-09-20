#include "feed_handler/capture_set.h"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "feed_handler/deribit/deribit_capture.h"
#include "feed_handler/kraken/kraken_capture.h"

namespace feed_handler {

namespace {

// Local runtime state, gitignored like ./journal: it belongs to this machine's
// process, not to the repository. The directory is the config's `state_dir`.
constexpr const char* kNonceStateFile = "kraken-nonce.state";

}  // namespace

std::expected<CaptureSet, std::string> CaptureSet::Build(
    const config::FeedHandlerConfig& config, std::span<const config::Connection> connections,
    std::span<const Credential> credentials) {
    if (connections.size() != credentials.size()) {
        return std::unexpected("internal error: " + std::to_string(connections.size()) +
                               " connections but " + std::to_string(credentials.size()) +
                               " credentials");
    }

    CaptureSet set;
    const bool any_kraken = std::ranges::any_of(connections, [](const config::Connection& c) {
        return c.exchange == config::Exchange::kKraken;
    });
    if (any_kraken) {
        // One RestClient for the whole process, shared by every Kraken
        // connection: it owns the nonce high-water mark that keeps signed calls
        // strictly increasing across reconnects (exchanges/kraken.md), so it
        // must never be rebuilt per connection, and it serializes their token
        // fetches. The state file extends that guarantee across restarts, best
        // effort and never a reason not to start (kraken_signing.h).
        set.rest_ = std::make_unique<kraken::RestClient>(
            std::string(kraken::RestClient::kDefaultBaseUrl), config.state_dir / kNonceStateFile);
    }

    for (std::size_t index = 0; index < connections.size(); ++index) {
        const config::Connection& connection = connections[index];
        switch (connection.exchange) {
            case config::Exchange::kKraken:
                set.connections_.push_back(
                    kraken::MakeKrakenCapture(config, connection, credentials[index], *set.rest_));
                set.kraken_connections_.push_back(connection);
                break;
            case config::Exchange::kDeribit:
                set.connections_.push_back(
                    deribit::MakeDeribitCapture(config, connection, credentials[index]));
                break;
        }
        set.exchanges_.push_back(connection.exchange);
    }
    return set;
}

void CaptureSet::StartAll() {
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        if (exchanges_[index] != config::Exchange::kKraken) {
            connections_[index]->Start();
        }
    }
    if (rest_ != nullptr) {
        kraken::LogInstrumentReference(*rest_, kraken_connections_);
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        if (exchanges_[index] == config::Exchange::kKraken) {
            connections_[index]->Start();
        }
    }
}

}  // namespace feed_handler
