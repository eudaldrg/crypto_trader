// How a config becomes live books: the startup check, the per-connection
// settings, and the CaptureSet::StartAll callback that plugs them in. No network:
// the Kraken reference data is parsed from a body the test holds, and the
// connections a real CaptureSet starts point at a refused loopback port.
#include "book_adapter/book_wiring.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_adapter/book_service.h"
#include "book_adapter/book_settings.h"
#include "feed_handler/capture_connection.h"
#include "feed_handler/capture_set.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/runner.h"
#include "feed_handler/tests/test_support.h"

namespace book_adapter {
namespace {

using feed_handler::CaptureSet;
using feed_handler::config::FeedHandlerConfig;
using feed_handler::test_support::DeribitEntry;
using feed_handler::test_support::kFakeCredential;
using feed_handler::test_support::KrakenEntry;
using feed_handler::test_support::MustParse;
using feed_handler::test_support::UniqueTestDir;

constexpr std::string_view kBooksOn = "order_books = true\n";
constexpr std::string_view kBothDecimals = "price_decimals = 1\nquantity_decimals = 0\n";

// Two pairs, trimmed to the fields the client reads (see kraken_rest_client_test).
constexpr std::string_view kAssetPairsBody =
    R"({"error":[],"result":{)"
    R"("XXBTZUSD":{"altname":"XBTUSD","wsname":"XBT/USD","pair_decimals":1,"lot_decimals":8,)"
    R"("tick_size":"0.1"},)"
    R"("XETHZUSD":{"altname":"ETHUSD","wsname":"ETH/USD","pair_decimals":2,"lot_decimals":8,)"
    R"("tick_size":"0.01"})"
    R"(}})";

// A connection that only remembers the sink it was given.
class SinkHoldingConnection final : public feed_handler::CaptureConnection {
  public:
    void AddSink(feed_handler::MessageSink& sink) override {
        sink_ = &sink;
    }
    std::string_view Id() const override {
        return "fake";
    }
    void Start() override {}
    void RequestStop() override {}
    void Join() override {}
    bool Fatal() const override {
        return false;
    }
    std::string Summary() const override {
        return "fake";
    }

    [[nodiscard]] feed_handler::MessageSink* Sink() const {
        return sink_;
    }

  private:
    feed_handler::MessageSink* sink_ = nullptr;
};

feed_handler::kraken::RestClient& LoadedRestClient() {
    static feed_handler::kraken::RestClient client;
    static const bool kLoaded = client.ParseAssetPairs(kAssetPairsBody).has_value();
    EXPECT_TRUE(kLoaded);
    return client;
}

TEST(BookServiceWiring, TheCheckIsAlwaysOkWhenBooksAreOff) {
    const FeedHandlerConfig config = MustParse(DeribitEntry("deribit-a"));
    EXPECT_TRUE(CheckLiveBookConfig(config, config.connections).has_value());
}

TEST(BookServiceWiring, ADeribitConnectionWithoutBothDecimalsIsRefusedNamingWhatIsMissing) {
    const FeedHandlerConfig neither = MustParse(std::string(kBooksOn) + DeribitEntry("deribit-a"));
    const auto refused = CheckLiveBookConfig(neither, neither.connections);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("[deribit-a]"), std::string::npos) << refused.error();
    EXPECT_NE(refused.error().find("price_decimals"), std::string::npos) << refused.error();
    EXPECT_NE(refused.error().find("quantity_decimals"), std::string::npos) << refused.error();

    const FeedHandlerConfig one = MustParse(
        std::string(kBooksOn) + DeribitEntry("deribit-b", "BTC-PERPETUAL", "price_decimals = 1\n"));
    const auto half = CheckLiveBookConfig(one, one.connections);
    ASSERT_FALSE(half.has_value());
    EXPECT_EQ(half.error().find("price_decimals"), std::string::npos) << half.error();
    EXPECT_NE(half.error().find("quantity_decimals"), std::string::npos) << half.error();
}

TEST(BookServiceWiring, TheCheckPassesWithBothDecimalsAndForKrakenAndOnlyLooksAtWhatIsSelected) {
    const FeedHandlerConfig config =
        MustParse(std::string(kBooksOn) + KrakenEntry("kraken-a") +
                  DeribitEntry("deribit-ok", "BTC-PERPETUAL", std::string(kBothDecimals)) +
                  DeribitEntry("deribit-bad"));
    // A Kraken entry needs nothing here.
    EXPECT_TRUE(
        CheckLiveBookConfig(config, std::span(config.connections).subspan(0, 1)).has_value());
    EXPECT_TRUE(
        CheckLiveBookConfig(config, std::span(config.connections).subspan(1, 1)).has_value());
    // The bad entry is only a problem when it is one of the selected connections
    // (--only / --exchange narrow what runs).
    EXPECT_FALSE(
        CheckLiveBookConfig(config, std::span(config.connections).subspan(2, 1)).has_value());
    EXPECT_FALSE(CheckLiveBookConfig(config, config.connections).has_value());
}

TEST(BookServiceWiring, DeribitSettingsComeFromTheConfigDecimals) {
    const FeedHandlerConfig config = MustParse(
        DeribitEntry("deribit-a", "BTC-PERPETUAL", "price_decimals = 2\nquantity_decimals = 3\n"));
    const auto settings = ResolveBookSettings(config.connections[0], nullptr);
    ASSERT_TRUE(settings.has_value()) << settings.error();
    EXPECT_EQ(settings->source, feed_handler::FrameSource::kDeribitFix);
    EXPECT_EQ(settings->scale.ToPrice(1.25).Value(), 125);
    EXPECT_EQ(settings->scale.ToQuantity(1.5).Lots(), 1500);
}

TEST(BookServiceWiring, KrakenSettingsTakeTheDepthFromTheConfigAndTheScaleFromAssetPairs) {
    const FeedHandlerConfig config = MustParse(KrakenEntry("kraken-a", "BTC/USD", "depth = 100\n"));
    const auto settings = ResolveBookSettings(config.connections[0], &LoadedRestClient());
    ASSERT_TRUE(settings.has_value()) << settings.error();
    EXPECT_EQ(settings->source, feed_handler::FrameSource::kKrakenJson);
    EXPECT_EQ(settings->kraken_depth, 100U);
    EXPECT_EQ(settings->scale.ToPrice(1.5).Value(), 15);
    EXPECT_EQ(settings->scale.ToQuantity(1.0).Lots(), 100'000'000);
}

TEST(BookServiceWiring, ManySymbolsOnOneKrakenConnectionGetTheFinestDecimalsAmongThem) {
    FeedHandlerConfig config = MustParse(KrakenEntry("kraken-a", "BTC/USD"));
    config.connections[0].symbols = {"BTC/USD", "ETH/USD"};
    const auto settings = ResolveBookSettings(config.connections[0], &LoadedRestClient());
    ASSERT_TRUE(settings.has_value()) << settings.error();
    // ETH/USD is the finer of the two: two price decimals.
    EXPECT_EQ(settings->scale.ToPrice(1.5).Value(), 150);
}

TEST(BookServiceWiring, ASymbolTheLookupDoesNotKnowIsAnErrorNotAGuess) {
    FeedHandlerConfig config = MustParse(KrakenEntry("kraken-a", "BTC/USD"));
    config.connections[0].symbols = {"BTC/USD", "DOGE/MOON"};
    const auto settings = ResolveBookSettings(config.connections[0], &LoadedRestClient());
    ASSERT_FALSE(settings.has_value());
    EXPECT_NE(settings.error().find("DOGE/MOON"), std::string::npos) << settings.error();

    // No lookup at all (it failed, or there is no RestClient).
    EXPECT_FALSE(ResolveBookSettings(config.connections[0], nullptr).has_value());
    const feed_handler::kraken::RestClient empty;
    EXPECT_FALSE(ResolveBookSettings(config.connections[0], &empty).has_value());
}

TEST(BookServiceWiring, BooksOffGiveNoCallbackAndNoThread) {
    const FeedHandlerConfig config = MustParse(DeribitEntry("deribit-a"));
    BookService service(BookService::Config{});
    EXPECT_FALSE(LiveBooksHook(config, service));
    service.Start();
    EXPECT_FALSE(service.Running());
    EXPECT_EQ(service.ConnectionCount(), 0U);
}

TEST(BookServiceWiring, TheCallbackRegistersASinkAndTheConnectionsEventsReachTheBooks) {
    const FeedHandlerConfig config =
        MustParse(std::string(kBooksOn) +
                  DeribitEntry("deribit-a", "BTC-PERPETUAL", std::string(kBothDecimals)));
    BookService service(BookService::Config{});
    const CaptureSet::BeforeStart hook = LiveBooksHook(config, service);
    ASSERT_TRUE(hook);

    SinkHoldingConnection connection;
    hook(connection, config.connections[0], nullptr);
    ASSERT_EQ(service.ConnectionCount(), 1U);
    ASSERT_NE(connection.Sink(), nullptr);

    // What the session does through the sink it was given.
    connection.Sink()->OnConnect(1, "test");
    connection.Sink()->OnDisconnect(1);
    service.Stop();
    EXPECT_EQ(service.Adapter().Name(service.Handle(0)), "deribit-a");
    EXPECT_EQ(service.Adapter().Stats(service.Handle(0)).connects, 1U);
    EXPECT_EQ(service.Adapter().Stats(service.Handle(0)).disconnects, 1U);
}

TEST(BookServiceWiring, AConnectionThatCannotHaveBooksIsCapturedWithoutThemNotRefused) {
    FeedHandlerConfig config = MustParse(std::string(kBooksOn) + KrakenEntry("kraken-a"));
    config.connections[0].symbols = {"DOGE/MOON"};
    BookService service(BookService::Config{});
    const CaptureSet::BeforeStart hook = LiveBooksHook(config, service);
    ASSERT_TRUE(hook);

    SinkHoldingConnection connection;
    hook(connection, config.connections[0], &LoadedRestClient());  // The lookup has no DOGE/MOON.
    EXPECT_EQ(service.ConnectionCount(), 0U);
    EXPECT_EQ(connection.Sink(), nullptr);
    service.Start();
    EXPECT_FALSE(service.Running());
}

// StartAll offers every connection to the callback before starting it, so the
// sinks are in place before a single frame can arrive. Deribit only: a Kraken
// entry would make StartAll run the real AssetPairs request.
class BookServiceWiringStartAll : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = UniqueTestDir("book_wiring");
    }
    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    // Two Deribit connections to a refused loopback port, started through the
    // callback for `body`'s config, run until a few polls have passed and stopped.
    static void StartAndStop(const FeedHandlerConfig& config, BookService& service) {
        std::vector<feed_handler::ResolvedConnection> resolved;
        resolved.reserve(config.connections.size());
        for (const auto& connection : config.connections) {
            resolved.push_back({.connection = connection, .credential = kFakeCredential});
        }
        auto set = CaptureSet::Build(config, std::move(resolved));
        ASSERT_TRUE(set.has_value()) << set.error();
        set->StartAll(LiveBooksHook(config, service));
        service.Start();
        int polls = 0;
        const auto result = feed_handler::Run(set->Connections(),
                                              {.poll = std::chrono::milliseconds{5},
                                               .should_stop = [&polls] { return ++polls > 2; }});
        EXPECT_FALSE(result.Fatal());
        // The order the binary uses: connections joined by Run, then the books.
        service.Stop();
    }

    std::string Preamble() const {
        return "journal_dir = \"" + (dir_ / "journal").string() + "\"\nstate_dir = \"" +
               (dir_ / "state").string() + "\"\n";
    }

    std::filesystem::path dir_;
};

TEST_F(BookServiceWiringStartAll, EveryConnectionGetsABookRingInTheOrderStartAllVisitsThem) {
    const FeedHandlerConfig config =
        MustParse(Preamble() + std::string(kBooksOn) +
                  DeribitEntry("deribit-a", "BTC-PERPETUAL",
                               "endpoint = \"127.0.0.1:1\"\n" + std::string(kBothDecimals)) +
                  DeribitEntry("deribit-b", "ETH-PERPETUAL",
                               "endpoint = \"127.0.0.1:1\"\n" + std::string(kBothDecimals)));
    BookService service(BookService::Config{});
    StartAndStop(config, service);

    ASSERT_EQ(service.ConnectionCount(), 2U);
    EXPECT_EQ(service.Adapter().Name(service.Handle(0)), "deribit-a");
    EXPECT_EQ(service.Adapter().Name(service.Handle(1)), "deribit-b");
    EXPECT_FALSE(service.Running());
}

TEST_F(BookServiceWiringStartAll, WithBooksOffTheSameRunCreatesNoRingAndNoThread) {
    const FeedHandlerConfig config = MustParse(
        Preamble() + DeribitEntry("deribit-a", "BTC-PERPETUAL", "endpoint = \"127.0.0.1:1\"\n"));
    BookService service(BookService::Config{});
    StartAndStop(config, service);

    EXPECT_EQ(service.ConnectionCount(), 0U);
    EXPECT_FALSE(service.Running());
}

}  // namespace
}  // namespace book_adapter
