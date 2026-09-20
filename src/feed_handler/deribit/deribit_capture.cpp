#include "feed_handler/deribit/deribit_capture.h"

#include <string>
#include <utility>

#include "feed_handler/capture_session.h"
#include "feed_handler/deribit/deribit_fix_client.h"
#include "feed_handler/deribit/deribit_fix_session.h"

namespace feed_handler::deribit {

namespace {

class DeribitCapture final : public CaptureConnection {
  public:
    DeribitCapture(const config::FeedHandlerConfig& config, const config::Connection& connection,
                   Credential credential)
        : id_(connection.id),
          session_({.directory = config.journal_dir,
                    .exchange = std::string(config::ToString(connection.exchange)),
                    .file_prefix = connection.id}),
          client_(SessionConfig{.client_id = std::move(credential.key),
                                .client_secret = std::move(credential.secret)},
                  session_,
                  FixClientConfig{.id = connection.id,
                                  .host = connection.host_port.host,
                                  .port = connection.host_port.port,
                                  .symbols = connection.symbols}) {}

    ~DeribitCapture() override {
        DeribitCapture::Join();
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
        // The connection thread sends the Logout on its own way out, so the
        // socket is never touched from here.
        client_.Join();
        session_.Close();
    }

    bool Fatal() const override {
        return client_.Fatal();
    }

    std::string Summary() const override {
        return "[" + id_ + "] captured " + std::to_string(client_.MessagesReceived()) +
               " messages (" + std::to_string(client_.SnapshotsReceived()) + " snapshot(s), " +
               std::to_string(client_.IncrementalsReceived()) + " incremental(s)) across " +
               std::to_string(session_.Incarnation()) + " incarnation(s), " +
               std::to_string(session_.TotalRecordsWritten()) + " journal records, " +
               std::to_string(client_.ConnectionAttempts()) + " connection attempt(s), " +
               std::to_string(client_.ForcedReconnects()) + " watchdog-forced reconnect(s)";
    }

  private:
    std::string id_;
    // Declared before the client that journals into it, so it is destroyed
    // after it.
    CaptureSession session_;
    FixClient client_;
};

}  // namespace

std::unique_ptr<CaptureConnection> MakeDeribitCapture(const config::FeedHandlerConfig& config,
                                                      const config::Connection& connection,
                                                      Credential credential) {
    return std::make_unique<DeribitCapture>(config, connection, std::move(credential));
}

}  // namespace feed_handler::deribit
