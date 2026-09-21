#include "feed_handler/kraken/kraken_capture.h"

#include <string>
#include <utility>

#include "feed_handler/client_capture.h"
#include "feed_handler/kraken/kraken_ws_client.h"
#include "feed_handler/logging.h"

namespace feed_handler::kraken {

namespace {

class KrakenCapture final : public ClientCapture<WsClient> {
  public:
    KrakenCapture(const config::FeedHandlerConfig& config, const config::Connection& connection,
                  Credential credential, RestClient& rest)
        : ClientCapture(config, connection, [&](CaptureSession& session) {
              return WsClient(rest,
                              Credentials{.api_key = std::move(credential.key),
                                          .api_secret_b64 = std::move(credential.secret)},
                              session,
                              WsClientConfig{.url = connection.endpoint,
                                             .id = connection.id,
                                             .symbols = connection.symbols});
          }) {}

    std::string Summary() const override {
        return "[" + std::string(Id()) + "] captured " +
               std::to_string(GetClient().MessagesReceived()) + " messages across " +
               std::to_string(JournalSession().ConnectId()) + " connect(s), " +
               std::to_string(JournalSession().TotalRecordsWritten()) + " journal records, " +
               std::to_string(GetClient().ForcedReconnects()) + " watchdog-forced reconnect(s)";
    }
};

}  // namespace

std::unique_ptr<CaptureConnection> MakeKrakenCapture(const config::FeedHandlerConfig& config,
                                                     const config::Connection& connection,
                                                     Credential credential, RestClient& rest) {
    return std::make_unique<KrakenCapture>(config, connection, std::move(credential), rest);
}

void LogInstrumentReference(RestClient& rest, std::span<const config::Connection> connections) {
    const auto loaded = rest.LoadAssetPairs();
    if (!loaded) {
        LogWarn("AssetPairs lookup failed, continuing without reference data: " + loaded.error());
        return;
    }
    LogInfo("loaded " + std::to_string(*loaded) + " asset pairs");
    for (const config::Connection& connection : connections) {
        if (connection.exchange != config::Exchange::kKraken) {
            continue;
        }
        const TaggedLog log(connection.id);
        for (const std::string& symbol : connection.symbols) {
            const AssetPair* pair = rest.FindAssetPair(symbol);
            if (pair == nullptr) {
                // The XBT/BTC trap: REST reports wsname "XBT/USD" for the
                // instrument WS v2 calls "BTC/USD" (exchanges/kraken.md).
                log.Warn("no AssetPairs entry for " + symbol +
                         ", continuing without reference data");
                continue;
            }
            log.Info(symbol + " is " + pair->rest_name + " tick_size=" + pair->tick_size +
                     " price_decimals=" + std::to_string(pair->price_decimals) +
                     " lot_decimals=" + std::to_string(pair->qty_decimals));
        }
    }
}

}  // namespace feed_handler::kraken
