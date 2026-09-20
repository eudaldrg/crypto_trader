#include "feed_handler/stop_signal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::StopSignal;
using feed_handler::test_support::ElapsedSince;
using std::chrono::milliseconds;

// Far longer than any wait below should last: a test that hits it has failed.
constexpr milliseconds kLongWait{10'000};
constexpr milliseconds kPrompt{2'000};
constexpr int kRaceAttempts = 200;

TEST(StopSignal, NothingIsSetUntilSomethingHappens) {
    const StopSignal signal;
    EXPECT_FALSE(signal.StopRequested());
    EXPECT_FALSE(signal.Fatal());
}

TEST(StopSignal, OnlyTheFirstRequestReportsItMadeIt) {
    StopSignal signal;
    EXPECT_TRUE(signal.RequestStop());
    EXPECT_FALSE(signal.RequestStop());
    EXPECT_TRUE(signal.StopRequested());
    EXPECT_FALSE(signal.Fatal());
}

TEST(StopSignal, AWaitWithNothingToReportTimesOutFalse) {
    StopSignal signal;
    const auto before = std::chrono::steady_clock::now();
    EXPECT_FALSE(signal.WaitFor(milliseconds{20}));
    EXPECT_GE(ElapsedSince(before), milliseconds{20});
}

TEST(StopSignal, AWaitReturnsTrueAtOnceWhenAStopWasAlreadyRequested) {
    StopSignal signal;
    signal.RequestStop();
    const auto before = std::chrono::steady_clock::now();
    EXPECT_TRUE(signal.WaitFor(kLongWait));
    EXPECT_LT(ElapsedSince(before), kPrompt);
}

TEST(StopSignal, ALatchedFatalEndsWaitsWithoutBeingAStopRequest) {
    StopSignal signal;
    signal.LatchFatal();
    EXPECT_TRUE(signal.Fatal());
    EXPECT_FALSE(signal.StopRequested());
    EXPECT_TRUE(signal.WaitFor(kLongWait));
}

TEST(StopSignal, ARequestFromAnotherThreadWakesASleepingWaiterPromptly) {
    StopSignal signal;
    const auto before = std::chrono::steady_clock::now();
    std::thread requester([&signal] {
        std::this_thread::sleep_for(milliseconds{30});
        signal.RequestStop();
    });
    EXPECT_TRUE(signal.WaitFor(kLongWait));
    requester.join();
    EXPECT_LT(ElapsedSince(before), kPrompt);
}

TEST(StopSignal, AFatalFromAnotherThreadWakesASleepingWaiterPromptly) {
    StopSignal signal;
    const auto before = std::chrono::steady_clock::now();
    std::thread latcher([&signal] {
        std::this_thread::sleep_for(milliseconds{30});
        signal.LatchFatal();
    });
    EXPECT_TRUE(signal.WaitFor(kLongWait));
    latcher.join();
    EXPECT_LT(ElapsedSince(before), kPrompt);
}

TEST(StopSignal, ARequestRacingTheWaiterEnteringItsWaitIsNeverLost) {
    // A notify that lands between the waiter's predicate check and its wait
    // registering is delivered to nobody, and the waiter then sleeps out its
    // whole timeout. A real race cannot be hit on demand, so the property is
    // asserted the way the clients' own tests do: over many attempts, each
    // racing the request against the wait, none may take anywhere near the
    // timeout. (TSan in the pre-push gate also watches this.)
    for (int attempt = 0; attempt < kRaceAttempts; ++attempt) {
        StopSignal signal;
        const auto before = std::chrono::steady_clock::now();
        std::thread waiter([&signal] { signal.WaitFor(kLongWait); });
        signal.RequestStop();
        waiter.join();
        ASSERT_LT(ElapsedSince(before), kPrompt) << "attempt " << attempt << " slept out the wait";
    }
}

}  // namespace
