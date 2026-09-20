// The runner against fake connections that record what they were asked, in
// order. The second-signal _exit(130) in InstallSignalHandlers is deliberately
// not tested: it would kill the test process, and raising a signal here would
// leave process-wide handler state behind for every later test.
#include "feed_handler/runner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using feed_handler::CaptureConnection;
using feed_handler::RunOptions;

constexpr std::chrono::milliseconds kTinyPoll{1};

using Calls = std::vector<std::string>;

class FakeConnection final : public CaptureConnection {
  public:
    FakeConnection(std::string id, Calls& calls) : id_(std::move(id)), calls_(calls) {}

    void SetFatal(bool fatal) {
        fatal_ = fatal;
    }

    std::string_view Id() const override {
        return id_;
    }
    void Start() override {
        calls_.push_back(id_ + ":Start");
    }
    void RequestStop() override {
        calls_.push_back(id_ + ":RequestStop");
    }
    void Join() override {
        calls_.push_back(id_ + ":Join");
    }
    bool Fatal() const override {
        return fatal_;
    }
    std::string Summary() const override {
        return id_;
    }

  private:
    std::string id_;
    Calls& calls_;
    std::atomic<bool> fatal_{false};
};

struct Fixture {
    Calls calls;
    std::vector<std::unique_ptr<CaptureConnection>> connections;

    FakeConnection& Add(const std::string& id) {
        auto connection = std::make_unique<FakeConnection>(id, calls);
        FakeConnection& reference = *connection;
        connections.push_back(std::move(connection));
        return reference;
    }

    std::size_t Count(const std::string& call) const {
        return static_cast<std::size_t>(std::ranges::count(calls, call));
    }

    std::size_t IndexOf(const std::string& call) const {
        return static_cast<std::size_t>(std::ranges::find(calls, call) - calls.begin());
    }
};

/// A should_stop that turns true on its Nth call.
std::function<bool()> StopAfter(int polls) {
    return [remaining = polls]() mutable { return remaining-- <= 0; };
}

TEST(CaptureRunner, AStopRequestEndsCleanlyAndStopsEveryConnectionOnce) {
    Fixture fixture;
    fixture.Add("a");
    fixture.Add("b");
    fixture.Add("c");

    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(3)});

    EXPECT_FALSE(result.Fatal());
    EXPECT_TRUE(result.fatal_id.empty());
    for (const char* id : {"a", "b", "c"}) {
        EXPECT_EQ(fixture.Count(std::string(id) + ":RequestStop"), 1U) << id;
        EXPECT_EQ(fixture.Count(std::string(id) + ":Join"), 1U) << id;
    }
    // Starting is the caller's decision, never the runner's.
    EXPECT_EQ(fixture.Count("a:Start") + fixture.Count("b:Start") + fixture.Count("c:Start"), 0U);
}

TEST(CaptureRunner, AFatalConnectionLatchesItsIdAndStopsEveryConnection) {
    Fixture fixture;
    fixture.Add("a");
    fixture.Add("b").SetFatal(true);
    fixture.Add("c");

    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(1000)});

    EXPECT_TRUE(result.Fatal());
    EXPECT_EQ(result.fatal_id, "b");
    for (const char* id : {"a", "b", "c"}) {
        EXPECT_EQ(fixture.Count(std::string(id) + ":RequestStop"), 1U) << id;
        EXPECT_EQ(fixture.Count(std::string(id) + ":Join"), 1U) << id;
    }
}

TEST(CaptureRunner, EveryRequestStopPrecedesAnyJoin) {
    Fixture fixture;
    fixture.Add("a");
    fixture.Add("b");
    fixture.Add("c");

    feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(0)});

    std::size_t last_request = 0;
    std::size_t first_join = fixture.calls.size();
    for (std::size_t index = 0; index < fixture.calls.size(); ++index) {
        if (fixture.calls[index].ends_with(":RequestStop")) {
            last_request = index;
        } else if (fixture.calls[index].ends_with(":Join")) {
            first_join = std::min(first_join, index);
        }
    }
    EXPECT_LT(last_request, first_join) << "a Join ran before every stop had been requested";
    EXPECT_EQ(fixture.calls.size(), 6U);
}

TEST(CaptureRunner, AFatalAlreadyLatchedIsNoticedOnTheFirstPoll) {
    Fixture fixture;
    fixture.Add("a").SetFatal(true);
    int polls = 0;

    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = [&polls] {
                                                    ++polls;
                                                    return false;
                                                }});

    EXPECT_TRUE(result.Fatal());
    EXPECT_EQ(result.fatal_id, "a");
    EXPECT_EQ(polls, 0) << "it polled the stop request before looking at the connections";
}

TEST(CaptureRunner, AFatalWinsOverAStopRequestedAtTheSameTime) {
    Fixture fixture;
    fixture.Add("a").SetFatal(true);

    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(0)});

    EXPECT_TRUE(result.Fatal());
    EXPECT_EQ(result.fatal_id, "a");
}

TEST(CaptureRunner, TheFirstFatalInTheOrderGivenIsTheOneReported) {
    Fixture fixture;
    fixture.Add("a");
    fixture.Add("b").SetFatal(true);
    fixture.Add("c").SetFatal(true);

    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(5)});
    EXPECT_EQ(result.fatal_id, "b");
}

TEST(CaptureRunner, HonoursThePollIntervalBetweenChecks) {
    Fixture fixture;
    fixture.Add("a");
    constexpr std::chrono::milliseconds kPoll{10};

    // Three polls that say "keep going", then stop: at least three sleeps.
    const auto before = std::chrono::steady_clock::now();
    feed_handler::Run(fixture.connections, {.poll = kPoll, .should_stop = StopAfter(3)});
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_GE(elapsed, 3 * kPoll);
}

TEST(CaptureRunner, NoConnectionsStillEndsOnAStopRequest) {
    Fixture fixture;
    const auto result =
        feed_handler::Run(fixture.connections, {.poll = kTinyPoll, .should_stop = StopAfter(1)});
    EXPECT_FALSE(result.Fatal());
}

TEST(CaptureRunner, NoSignalHasArrivedInATestProcess) {
    EXPECT_FALSE(feed_handler::ShutdownRequested());
}

}  // namespace
