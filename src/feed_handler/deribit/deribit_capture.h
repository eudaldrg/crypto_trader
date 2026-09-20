// One Deribit FIX connection as a CaptureConnection: its journal session and
// its FIX client, built from a config entry.
#pragma once

#include <memory>

#include "feed_handler/capture_connection.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"

namespace feed_handler::deribit {

/// The credential's key is the Deribit client id and its secret the client
/// secret. The journal header carries the exchange tag "deribit"; the files are
/// named after the connection id.
std::unique_ptr<CaptureConnection> MakeDeribitCapture(const config::FeedHandlerConfig& config,
                                                      const config::Connection& connection,
                                                      Credential credential);

}  // namespace feed_handler::deribit
