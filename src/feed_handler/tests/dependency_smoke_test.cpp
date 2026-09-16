// Verifies IXWebSocket, simdjson and GoogleTest are actually fetched,
// configured and linkable together -- not real feed handler behavior.
#include <gtest/gtest.h>
#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>

TEST(DependencySmoke, IXWebSocketConstructs) {
    ix::WebSocket ws;
    ws.setUrl("wss://example.invalid");
    EXPECT_EQ(ws.getUrl(), "wss://example.invalid");
}

TEST(DependencySmoke, SimdjsonParsesTrivialDocument) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string json =
        simdjson::padded_string(std::string(R"({"channel":"level3","type":"snapshot"})"));
    simdjson::ondemand::document doc = parser.iterate(json);
    std::string_view channel = doc["channel"].get_string().value();
    EXPECT_EQ(channel, "level3");
}
