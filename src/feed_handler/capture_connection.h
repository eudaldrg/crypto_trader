// The control-plane view of one running capture, whichever exchange it is for.
//
// A process that runs several connections needs to start them, ask them to
// stop, wait for them, and ask whether one has failed -- and nothing else. This
// is that, and only that: it says nothing about how a connection reads or
// journals its messages, which stays exchange-specific and on the per-message
// path. Virtual dispatch is fine here because it is a handful of calls per
// process (ADR 0006's compile-time preference is about the per-message path).
#pragma once

#include <string>
#include <string_view>

namespace feed_handler {

class CaptureConnection {
  public:
    CaptureConnection() = default;
    CaptureConnection(const CaptureConnection&) = delete;
    CaptureConnection& operator=(const CaptureConnection&) = delete;
    CaptureConnection(CaptureConnection&&) = delete;
    CaptureConnection& operator=(CaptureConnection&&) = delete;

    /// Implementations must stop, join and close their journal here, idempotently:
    /// a connection abandoned by an early return must not leave a thread running
    /// or a journal unflushed.
    virtual ~CaptureConnection() = default;

    /// The config's connection id, which also names the journal files and tags
    /// every log line.
    virtual std::string_view Id() const = 0;

    /// Starts the connection's thread(s) and returns immediately.
    virtual void Start() = 0;

    /// Asks the connection to stop and returns without waiting.
    virtual void RequestStop() = 0;

    /// Waits until the connection's thread(s) have ended, then closes its
    /// journal. Requests the stop first if nobody has. Idempotent.
    virtual void Join() = 0;

    /// True once capture cannot continue: a journal file could not be opened, or
    /// a write into an open one failed.
    virtual bool Fatal() const = 0;

    /// One line for the log at shutdown: what this connection captured, tagged
    /// with its id. Exchange-specific, since the counters differ.
    virtual std::string Summary() const = 0;
};

}  // namespace feed_handler
