// The part of a CaptureConnection that is the same for every exchange: one
// journal session and one client, started, stopped, joined and closed together.
//
// The journal runs on its own thread (JournalMode::kThreaded): the client's
// connection thread never touches the disk, and a journal failure reaches the
// client through the fatal handler it registers on the session.
//
// The teardown contract lives here once: Join() joins the client and then closes
// the session (a barrier for the journal thread, so the file is complete and no
// fatal can still arrive), the destructor calls it, and the session is declared
// before the client that journals into it so it is destroyed after it. An
// exchange derives, says how to build its client, and adds its own Summary().
#pragma once

#include <string>
#include <string_view>

#include "feed_handler/capture_connection.h"
#include "feed_handler/capture_session.h"
#include "feed_handler/config/feed_handler_config.h"

namespace feed_handler {

/// `Client` needs Start(), RequestStop(), Join() and Fatal(). It may be neither
/// copyable nor movable: it is built in place from the callable given to the
/// constructor.
template <class Client>
class ClientCapture : public CaptureConnection {
  public:
    ClientCapture(const ClientCapture&) = delete;
    ClientCapture& operator=(const ClientCapture&) = delete;
    ClientCapture(ClientCapture&&) = delete;
    ClientCapture& operator=(ClientCapture&&) = delete;

    ~ClientCapture() override {
        ClientCapture::Join();
    }

    std::string_view Id() const override {
        return id_;
    }

    void AddSink(MessageSink& sink) override {
        session_.AddSink(sink);
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

  protected:
    /// `make_client(CaptureSession&)` returns the client by value (a prvalue, so
    /// a non-movable one is built in place). The journal header carries the
    /// exchange tag, the files are named after the connection id. The client is
    /// given a threaded session and must register its fatal handler on it
    /// (CaptureSession::SetFatalHandler) before it starts.
    template <class MakeClient>
    ClientCapture(const config::FeedHandlerConfig& config, const config::Connection& connection,
                  MakeClient&& make_client)
        : id_(connection.id),
          session_({.directory = config.journal_dir,
                    .exchange = std::string(config::ToString(connection.exchange)),
                    .file_prefix = connection.id,
                    .journal_mode = JournalMode::kThreaded}),
          client_(make_client(session_)) {}

    const CaptureSession& JournalSession() const {
        return session_;
    }

    /// Mutable access for a derived class that drives the session and the client
    /// directly, which is how a test gets frames in without a live socket.
    CaptureSession& JournalSession() {
        return session_;
    }

    const Client& GetClient() const {
        return client_;
    }

    Client& GetClient() {
        return client_;
    }

  private:
    std::string id_;
    // Declared before the client that journals into it, so it is destroyed
    // after it.
    CaptureSession session_;
    Client client_;
};

}  // namespace feed_handler
