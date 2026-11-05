// Every capture connection of one process, and the order they come up and go
// down in.
//
// Owns what the connections share (the Kraken RestClient) and the connections
// themselves, so that "the RestClient outlives every Kraken client" is a
// property of the member order below rather than a comment in main().
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "feed_handler/capture_connection.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/kraken/kraken_rest_client.h"

namespace feed_handler {

class CaptureSet {
  public:
    /// Builds one connection per entry of `connections`, in order, each with the
    /// credential it was resolved with. Starts nothing and touches no network. A
    /// RestClient is created only if a Kraken connection is selected, with its
    /// nonce state file in `config.state_dir`. The credentials are moved into the
    /// connections, so the set does not keep a second copy of a secret.
    static std::expected<CaptureSet, std::string> Build(
        const config::FeedHandlerConfig& config, std::vector<ResolvedConnection> connections);

    CaptureSet(CaptureSet&&) = default;
    CaptureSet& operator=(CaptureSet&&) = delete;
    CaptureSet(const CaptureSet&) = delete;
    CaptureSet& operator=(const CaptureSet&) = delete;

    /// Stops and joins every connection (their destructors do), then the
    /// RestClient goes: see the member order.
    ~CaptureSet() = default;

    /// Called by StartAll once per connection, immediately before that
    /// connection's Start(), on the thread that called StartAll, with the
    /// connection, the config entry it was built from and, for a Kraken
    /// connection only (null otherwise), the shared RestClient whose AssetPairs
    /// cache the instrument-reference lookup has just filled (FindAssetPair finds
    /// nothing if the lookup failed; reading it is safe, no Kraken connection
    /// thread exists yet).
    ///
    /// The one place a consumer downstream of the capture (the order books, which
    /// live in a library that depends on this one) can attach to a connection with
    /// what only this order knows: `AddSink` is only legal before Start(), and a
    /// Kraken connection's instrument scale exists only after the lookup, which
    /// runs after the Deribit connections have started. It must not throw and must
    /// not block for long: the connection waits for it.
    using BeforeStart =
        std::function<void(CaptureConnection& connection, const config::Connection& entry,
                           const kraken::RestClient* rest)>;

    /// Starts every non-Kraken connection, then the Kraken instrument-reference
    /// lookup, then the Kraken connections, calling `before_start` (if given)
    /// ahead of each connection's Start(). That order is the point:
    ///  - The lookup is a blocking REST GET (10 s connect and 20 s transfer
    ///    timeouts, so up to about 30 s). Run before anything starts, a slow
    ///    Kraken REST endpoint would delay Deribit's capture.
    ///  - It cannot simply run after the Kraken connections start: it takes the
    ///    RestClient's request mutex, the same one every token fetch takes, so
    ///    it would stall connections that are mid-connect, and FindAssetPair
    ///    reads the pair table without synchronisation by contract.
    void StartAll(const BeforeStart& before_start = {});

    /// In the order the entries were given.
    std::span<const std::unique_ptr<CaptureConnection>> Connections() const {
        return connections_;
    }

    /// Whether a Kraken RestClient exists, i.e. whether any Kraken connection
    /// was selected.
    bool HasRestClient() const {
        return rest_ != nullptr;
    }

  private:
    CaptureSet() = default;

    // ORDER MATTERS: members are destroyed in reverse, so `connections_` (whose
    // destructors join the threads that use `*rest_`) goes before `rest_`. A
    // unique_ptr, not an optional: RestClient is neither copyable nor movable,
    // and this set has to be returned by value.
    std::unique_ptr<kraken::RestClient> rest_;
    std::vector<std::unique_ptr<CaptureConnection>> connections_;
    /// The entries the connections were built from, parallel to `connections_`:
    /// what StartAll needs to tell Kraken from the rest and to log the Kraken
    /// instrument reference.
    std::vector<config::Connection> entries_;
};

}  // namespace feed_handler
