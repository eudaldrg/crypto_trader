// The application-level staleness timer decisions/0004 requires, exercised
// against an injected clock rather than by sleeping: the whole reason the
// watchdog takes `now_ns` as a parameter is that its timing is testable
// without a live socket or a slow test.
#include "feed_handler/staleness_watchdog.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using feed_handler::staleness_watchdog;

constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kTimeout = 30 * kSecond;

}  // namespace

TEST(StalenessWatchdog, StartsDisarmedSoSilenceBeforeTheFirstMessageIsNotStaleness) {
    const staleness_watchdog watchdog(kTimeout);
    EXPECT_FALSE(watchdog.armed());
    EXPECT_FALSE(watchdog.is_stale(kTimeout * 100));
}

TEST(StalenessWatchdog, FiresOnlyOnceTheTimeoutHasFullyElapsed) {
    staleness_watchdog watchdog(kTimeout);
    watchdog.note_activity(1'000 * kSecond);

    EXPECT_FALSE(watchdog.is_stale(1'000 * kSecond));
    EXPECT_FALSE(watchdog.is_stale((1'000 * kSecond) + kTimeout - 1));
    EXPECT_TRUE(watchdog.is_stale((1'000 * kSecond) + kTimeout));
    EXPECT_TRUE(watchdog.is_stale((1'000 * kSecond) + (2 * kTimeout)));
}

TEST(StalenessWatchdog, TrafficPushesTheDeadlineOut) {
    staleness_watchdog watchdog(kTimeout);
    watchdog.note_activity(kSecond);
    EXPECT_TRUE(watchdog.is_stale(kSecond + kTimeout));

    // A heartbeat arriving late still counts: any inbound byte proves the
    // connection is not half-open.
    watchdog.note_activity(kSecond + kTimeout);
    EXPECT_FALSE(watchdog.is_stale(kSecond + kTimeout + 1));
    EXPECT_TRUE(watchdog.is_stale(kSecond + (2 * kTimeout)));
}

TEST(StalenessWatchdog, DisarmStopsItFiringRepeatedlyWhileReconnecting) {
    staleness_watchdog watchdog(kTimeout);
    watchdog.note_activity(kSecond);
    ASSERT_TRUE(watchdog.is_stale(kSecond + kTimeout));

    // What the client does the moment it forces a reconnect. Without this the
    // watchdog would keep firing through the whole reconnect, tearing down
    // each new connection before it could deliver its first message.
    watchdog.disarm();
    EXPECT_FALSE(watchdog.is_stale(kSecond + (10 * kTimeout)));

    // Rearmed by the first traffic on the new connection.
    watchdog.note_activity(kSecond + (10 * kTimeout));
    EXPECT_TRUE(watchdog.armed());
    EXPECT_FALSE(watchdog.is_stale(kSecond + (10 * kTimeout) + 1));
}

TEST(StalenessWatchdog, ZeroTimeoutDisablesTheCheck) {
    staleness_watchdog watchdog(0);
    watchdog.note_activity(kSecond);
    EXPECT_FALSE(watchdog.is_stale(kSecond * 1'000'000));
}

TEST(StalenessWatchdog, IgnoresATimestampOlderThanTheLastActivity) {
    // CLOCK_MONOTONIC cannot go backwards, but an unsigned subtraction that
    // wrapped would report staleness of ~584 years, so the guard is worth
    // pinning down.
    staleness_watchdog watchdog(kTimeout);
    watchdog.note_activity(1'000 * kSecond);
    EXPECT_FALSE(watchdog.is_stale(500 * kSecond));
}
