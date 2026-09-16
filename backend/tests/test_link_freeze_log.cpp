/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Link freezes counted for the stats card (LinkFreezeLog.h).
 *
 * One freeze is seen twice — the video backlog going down, the client's
 * silence coming up — and again on every frame shed while it lasts. What
 * matters is that all of that reads as ONE freeze with its real length, and
 * that two freezes seconds apart stay two.
 */

#include "../src/streaming/LinkFreezeLog.h"

#include "test_framework.h"

void run_link_freeze_log_tests()
{
    SECTION("LinkFreezeLog — nothing noted, nothing to show");
    {
        LinkFreezeLog log;
        const LinkFreezeLog::Snapshot s = log.snapshot();
        CHECK_EQ(s.count, 0);
        CHECK_EQ(s.maxMs, int64_t(0));
        CHECK_EQ(s.lastMs, int64_t(0));
    }

    SECTION("LinkFreezeLog — both directions and every shed frame are one freeze");
    {
        // The corporate Wi-Fi of 16/09/2026, 15:47:26: the backlog aged past
        // its tolerance, frames were shed as it kept growing, and the client
        // went silent over the same second.
        LinkFreezeLog log;
        log.note(10000, 10261); // first shed frame, backlog 261 ms
        log.note(10000, 10310); // the next ones stretch it
        log.note(10050, 10407); // the client's silence, up
        log.note(10000, 11232); // the last shed keyframe
        const LinkFreezeLog::Snapshot s = log.snapshot();
        CHECK_EQ(s.count, 1);
        CHECK_EQ(s.maxMs, int64_t(1232));
        CHECK_EQ(s.lastMs, int64_t(1232));
    }

    SECTION("LinkFreezeLog — freezes seconds apart stay apart");
    {
        LinkFreezeLog log;
        log.note(10000, 10900);
        // Within a second of the end: still the same one.
        log.note(11800, 11900);
        CHECK_EQ(log.snapshot().count, 1);
        CHECK_EQ(log.snapshot().maxMs, int64_t(1900));
        // Past it: a second freeze, shorter — the longest stays.
        log.note(20000, 20300);
        const LinkFreezeLog::Snapshot s = log.snapshot();
        CHECK_EQ(s.count, 2);
        CHECK_EQ(s.maxMs, int64_t(1900));
        CHECK_EQ(s.lastMs, int64_t(300));
    }

    SECTION("LinkFreezeLog — a span that ends before it starts is ignored");
    {
        LinkFreezeLog log;
        log.note(500, 400);
        CHECK_EQ(log.snapshot().count, 0);
    }
}
