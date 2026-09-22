/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The host's own share of the receiver's link report (HostLagTracker.h).
 *
 * A slower encode must come out of the "link queue" the receiver reports,
 * and nothing else may: no frames means nothing to take out, and a lag that
 * has been the norm for thirty seconds becomes the new best, as it does on
 * the receiver's side.
 */

#include "../src/streaming/HostLagTracker.h"

#include "test_framework.h"

void run_host_lag_tracker_tests()
{
    SECTION("HostLagTracker — no frames, nothing to take out");
    {
        HostLagTracker t;
        CHECK_EQ(t.takeRise(1000), int64_t(0));
    }

    SECTION("HostLagTracker — the encode slowing under load is the host's rise");
    {
        // DualRTX, 22/09/2026: ~5 ms from capture to the wire when the Arc was
        // free, ~19 ms once a game-like load took it.
        HostLagTracker t;
        t.note(5, 1000);
        t.note(6, 1016);
        CHECK_EQ(t.takeRise(1500), int64_t(0));
        t.note(19, 2000);
        t.note(21, 2016);
        CHECK_EQ(t.takeRise(2500), int64_t(14));
        // The window closed: the next one is judged on its own frames.
        t.note(7, 3000);
        CHECK_EQ(t.takeRise(3500), int64_t(2));
    }

    SECTION("HostLagTracker — a lag that fills a whole thirty-second stretch becomes the best");
    {
        HostLagTracker t;
        t.note(5, 0);
        CHECK_EQ(t.takeRise(500), int64_t(0));
        int64_t ms = 1000;
        for (; ms <= 31000; ms += 500) {
            t.note(19, ms);
            t.takeRise(ms + 1);
        }
        // The first stretch still held the 5 ms frame: still a rise.
        t.note(19, ms);
        CHECK_EQ(t.takeRise(ms + 1), int64_t(14));
        for (ms += 500; ms <= 62000; ms += 500) {
            t.note(19, ms);
            t.takeRise(ms + 1);
        }
        // A whole stretch at 19 ms: that is the best now, and no rise.
        t.note(19, ms);
        CHECK_EQ(t.takeRise(ms + 1), int64_t(0));
    }

    SECTION("HostLagTracker — a negative lag is a clock artefact and ignored");
    {
        HostLagTracker t;
        t.note(-3, 1000);
        CHECK_EQ(t.takeRise(1500), int64_t(0));
        t.note(8, 2000);
        CHECK_EQ(t.takeRise(2500), int64_t(0));
    }
}
