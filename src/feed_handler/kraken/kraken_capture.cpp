#include "feed_handler/kraken/kraken_capture.h"

#include <string>
#include <utility>

#include "feed_handler/capture_session.h"
#include "feed_handler/kraken/kraken_ws_client.h"
#include "feed_handler/logging.h"

namespace feed_handler::kraken {

namespace {

class KrakenCapture final : public CaptureConnection {
  public:
    KrakenCapture(const config::FeedHandlerConfig& config, const config::Connection& connection,
                  Credential credential, RestClient& rest)
        : id_(connection.id),
          session_({.directory = config.journal_dir,
                    .exchange = std::string(config::ToString(connection.exchange)),
                    .file_prefix = connection.id}),
          client_(rest,
                  Credentials{.api_key = std::move(credential.key),
                              .api_secret_b64 = std::move(credential.secret)},
                  session_,
                  WsClientConfig{.url = connection.endpoint,
                                 .id = connection.id,
                                 .symbols = connection.symbols}) {}

    ~KrakenCapture() override {
        KrakenCapture::Join();
    }

    std::string_view Id() const override {
        return id_;
    }

    void Start() override {
        client_.Start();
    }

    void RequestStop() override {
        client_.RequestStop();
    }

    void Join() override {
        client_.Join();
        session_.Close();
    }

    bool Fatal() const override {
        return client_.Fatal();
    }

    std::string Summary() const override {
        return "[" + id_ + "] captured " + std::to_string(client_.MessagesReceived()) +
               " messages across " + std::to_string(session_.Incarnation()) + " incarnation(s), " +
               std::to_string(session_.TotalRecordsWritten()) + " journal records, " +
               std::to_string(client_.ForcedReconnects()) + " watchdog-forced reconnect(s)";
    }

  private:
    std::string id_;
    // Declared before the client that journals into it, so it is destroyed
    // after it.
    CaptureSession session_;
    WsClient client_;
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
