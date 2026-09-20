#include "feed_handler/capture_set.h"

#include <filesystem>

#include "feed_handler/deribit/deribit_capture.h"
#include "feed_handler/kraken/kraken_capture.h"

namespace feed_handler {

namespace {

// Local runtime state, gitignored like ./journal: it belongs to this machine's
// process, not to the repository. The directory is the config's `state_dir`.
constexpr const char* kNonceStateFile = "kraken-nonce.state";

}  // namespace

std::expected<CaptureSet, std::string> CaptureSet::Build(
    const config::FeedHandlerConfig& config, std::vector<ResolvedConnection> connections) {
    CaptureSet set;
    for (ResolvedConnection& resolved : connections) {
        const config::Connection& connection = resolved.connection;
        switch (connection.exchange) {
            case config::Exchange::kKraken:
                if (set.rest_ == nullptr) {
                    // One RestClient for the whole process, shared by every
                    // Kraken connection: it owns the nonce high-water mark that
                    // keeps signed calls strictly increasing across reconnects
                    // (exchanges/kraken.md), so it must never be rebuilt per
                    // connection, and it serializes their token fetches. The
                    // state file extends that guarantee across restarts, best
                    // effort and never a reason not to start (kraken_signing.h).
                    set.rest_ = std::make_unique<kraken::RestClient>(
                        std::string(kraken::RestClient::kDefaultBaseUrl),
                        config.state_dir / kNonceStateFile);
                }
                set.connections_.push_back(kraken::MakeKrakenCapture(
                    config, connection, std::move(resolved.credential), *set.rest_));
                break;
            case config::Exchange::kDeribit:
                set.connections_.push_back(deribit::MakeDeribitCapture(
                    config, connection, std::move(resolved.credential)));
                break;
        }
        set.entries_.push_back(std::move(resolved.connection));
    }
    return set;
}

void CaptureSet::StartAll() {
    const auto start = [this](bool kraken) {
        for (std::size_t index = 0; index < connections_.size(); ++index) {
            if ((entries_[index].exchange == config::Exchange::kKraken) == kraken) {
                connections_[index]->Start();
            }
        }
    };
    start(false);
    if (rest_ != nullptr) {
        kraken::LogInstrumentReference(*rest_, entries_);
    }
    start(true);
}

}  // namespace feed_handler
