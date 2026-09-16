// The application-level staleness timer decisions/0004 requires, exercised
// against an injected clock rather than by sleeping: the whole reason the
// watchdog takes `now_ns` as a parameter is that its timing is testable
// without a live socket or a slow test.
#include "feed_handler/staleness_watchdog.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using feed_handler::StalenessWatchdog;

constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kTimeout = 30 * kSecond;

}  // namespace

TEST(StalenessWatchdog, StartsDisarmedSoSilenceBeforeTheFirstMessageIsNotStaleness) {
    const StalenessWatchdog watchdog(kTimeout);
    EXPECT_FALSE(watchdog.Armed());
    EXPECT_FALSE(watchdog.IsStale(kTimeout * 100));
}

TEST(StalenessWatchdog, FiresOnlyOnceTheTimeoutHasFullyElapsed) {
    StalenessWatchdog watchdog(kTimeout);
    watchdog.NoteActivity(1'000 * kSecond);

    EXPECT_FALSE(watchdog.IsStale(1'000 * kSecond));
    EXPECT_FALSE(watchdog.IsStale((1'000 * kSecond) + kTimeout - 1));
    EXPECT_TRUE(watchdog.IsStale((1'000 * kSecond) + kTimeout));
    EXPECT_TRUE(watchdog.IsStale((1'000 * kSecond) + (2 * kTimeout)));
}

TEST(StalenessWatchdog, TrafficPushesTheDeadlineOut) {
    StalenessWatchdog watchdog(kTimeout);
    watchdog.NoteActivity(kSecond);
    EXPECT_TRUE(watchdog.IsStale(kSecond + kTimeout));

    // A heartbeat arriving late still counts: any inbound byte proves the
    // connection is not half-open.
    watchdog.NoteActivity(kSecond + kTimeout);
    EXPECT_FALSE(watchdog.IsStale(kSecond + kTimeout + 1));
    EXPECT_TRUE(watchdog.IsStale(kSecond + (2 * kTimeout)));
}

TEST(StalenessWatchdog, DisarmStopsItFiringRepeatedlyWhileReconnecting) {
    StalenessWatchdog watchdog(kTimeout);
    watchdog.NoteActivity(kSecond);
    ASSERT_TRUE(watchdog.IsStale(kSecond + kTimeout));

    // What the client does the moment it forces a reconnect. Without this the
    // watchdog would keep firing through the whole reconnect, tearing down
    // each new connection before it could deliver its first message.
    watchdog.Disarm();
    EXPECT_FALSE(watchdog.IsStale(kSecond + (10 * kTimeout)));

    // Rearmed by the first traffic on the new connection.
    watchdog.NoteActivity(kSecond + (10 * kTimeout));
    EXPECT_TRUE(watchdog.Armed());
    EXPECT_FALSE(watchdog.IsStale(kSecond + (10 * kTimeout) + 1));
}

TEST(StalenessWatchdog, ZeroTimeoutDisablesTheCheck) {
    StalenessWatchdog watchdog(0);
    watchdog.NoteActivity(kSecond);
    EXPECT_FALSE(watchdog.IsStale(kSecond * 1'000'000));
}

TEST(StalenessWatchdog, IgnoresATimestampOlderThanTheLastActivity) {
    // CLOCK_MONOTONIC cannot go backwards, but an unsigned subtraction that
    // wrapped would report staleness of ~584 years, so the guard is worth
    // pinning down.
    StalenessWatchdog watchdog(kTimeout);
    watchdog.NoteActivity(1'000 * kSecond);
    EXPECT_FALSE(watchdog.IsStale(500 * kSecond));
}
