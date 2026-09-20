#include "feed_handler/deribit/deribit_capture.h"

#include <string>
#include <utility>

#include "feed_handler/client_capture.h"
#include "feed_handler/deribit/deribit_fix_client.h"
#include "feed_handler/deribit/deribit_fix_session.h"

namespace feed_handler::deribit {

namespace {

class DeribitCapture final : public ClientCapture<FixClient> {
  public:
    DeribitCapture(const config::FeedHandlerConfig& config, const config::Connection& connection,
                   Credential credential)
        : ClientCapture(config, connection, [&](CaptureSession& session) {
              return FixClient(SessionConfig{.client_id = std::move(credential.key),
                                             .client_secret = std::move(credential.secret)},
                               session,
                               FixClientConfig{.id = connection.id,
                                               .host = connection.host_port.host,
                                               .port = connection.host_port.port,
                                               .symbols = connection.symbols});
          }) {}

    std::string Summary() const override {
        return "[" + std::string(Id()) + "] captured " +
               std::to_string(GetClient().MessagesReceived()) + " messages (" +
               std::to_string(GetClient().SnapshotsReceived()) + " snapshot(s), " +
               std::to_string(GetClient().IncrementalsReceived()) + " incremental(s)) across " +
               std::to_string(JournalSession().Incarnation()) + " incarnation(s), " +
               std::to_string(JournalSession().TotalRecordsWritten()) + " journal records, " +
               std::to_string(GetClient().ConnectionAttempts()) + " connection attempt(s), " +
               std::to_string(GetClient().ForcedReconnects()) + " watchdog-forced reconnect(s)";
    }
};

}  // namespace

std::unique_ptr<CaptureConnection> MakeDeribitCapture(const config::FeedHandlerConfig& config,
                                                      const config::Connection& connection,
                                                      Credential credential) {
    return std::make_unique<DeribitCapture>(config, connection, std::move(credential));
}

}  // namespace feed_handler::deribit
