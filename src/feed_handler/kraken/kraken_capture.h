// One Kraken level3 connection as a CaptureConnection: its journal session and
// its WebSocket client, built from a config entry.
#pragma once

#include <memory>
#include <span>

#include "feed_handler/capture_connection.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/kraken/kraken_rest_client.h"

namespace feed_handler::kraken {

/// `rest` is shared by every Kraken connection in the process (it owns the
/// nonce high-water mark) and must outlive the returned connection. The journal
/// header carries the exchange tag "kraken"; the files are named after the
/// connection id.
std::unique_ptr<CaptureConnection> MakeKrakenCapture(const config::FeedHandlerConfig& config,
                                                     const config::Connection& connection,
                                                     Credential credential, RestClient& rest);

/// Fetches instrument reference data and logs each configured symbol's tick
/// size. Not blocking for capture: the journal holds raw bytes, and nothing
/// downstream of it needs a tick size yet (decisions/0004 keeps reference data
/// out of the journal). It doubles as an early typo check: a symbol with no
/// AssetPairs entry is logged, since Kraken would otherwise only reject it on
/// subscribe.
///
/// A blocking REST GET with no timeout, and it takes the client's request
/// mutex, so it must run before any Kraken connection starts (CaptureSet).
void LogInstrumentReference(RestClient& rest, std::span<const config::Connection> connections);

}  // namespace feed_handler::kraken
