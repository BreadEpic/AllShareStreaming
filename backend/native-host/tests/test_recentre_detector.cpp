/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/RecentreDetector.h"

#include <cstdio>
#include <string>

using namespace mw::native::input;

// A game that warps the pointer back to the middle of its window every frame,
// told from everything that does not — and, once told, the client's positions
// turned into the deltas that game measures.
//
// The failure this guards is total: with Counter-Strike (GoldSrc) or Portal 2
// on the host and the stream in desktop mode, every client position was placed
// on the screen, the game read the distance from its centre as a flick, and the
// view spun (18/09/2026, the log's alternating "pointer report JUMP" at 960,540
// and wherever the viewer's pointer was). Pure integers, tested on every
// platform: each host reads and places a pointer the same way.

namespace {

// One placement: the pointer was found where the last call put it (nothing
// moved it), the client wants it at (x, y).
RecentreDetector::Verdict placeUndisturbed(RecentreDetector& d, int64_t lastX, int64_t lastY,
                                           int64_t x, int64_t y, int64_t nowUs)
{
    return d.observe(true, lastX, lastY, x, y, nowUs);
}

} // namespace

void run_recentre_detector_tests()
{
    SECTION("Re-centre detector — a game warping the pointer, told from the desktop");

    // ── The desktop: the pointer stays where it is put ───────────────────────
    {
        RecentreDetector d;
        // The very first position has nothing to compare against.
        RecentreDetector::Verdict v = d.observe(true, 500, 500, 100, 100, 0);
        CHECK(!v.relative);
        CHECK(!v.changed);
        int64_t lx = 100;
        int64_t ly = 100;
        for (int i = 1; i <= 50; ++i) {
            v = placeUndisturbed(d, lx, ly, 100 + i * 7, 100 + i * 3, i * 8000);
            CHECK(!v.relative);
            CHECK(!v.changed);
            lx = 100 + i * 7;
            ly = 100 + i * 3;
        }
        CHECK(!d.relative());
    }

    // ── Rounding of the absolute axis is not "someone moved it" ─────────────
    {
        RecentreDetector d;
        d.observe(true, 0, 0, 100, 100, 0);
        for (int i = 1; i <= 10; ++i) {
            // Found two pixels off what was asked, each time.
            const RecentreDetector::Verdict v =
                d.observe(true, 100 + i * 5 - 5 + 2, 100 - 1, 100 + i * 5, 100, i * 8000);
            CHECK(!v.relative);
        }
    }

    // ── The game: three returns to one spot, then deltas ────────────────────
    {
        RecentreDetector d;
        const int64_t cx = 1280;
        const int64_t cy = 720;
        d.observe(true, 0, 0, 400, 900, 0); // first placement
        // Each time we look, the game has put the pointer back at its centre.
        RecentreDetector::Verdict v = d.observe(true, cx, cy, 410, 900, 8000);
        CHECK(!v.relative);
        v = d.observe(true, cx + 1, cy, 420, 900, 16000); // within tolerance
        CHECK(!v.relative);
        v = d.observe(true, cx, cy - 1, 430, 905, 24000);
        CHECK(v.relative);
        CHECK(v.changed);
        CHECK_EQ(v.deltaX, 10);
        CHECK_EQ(v.deltaY, 5);
        CHECK(d.relative());
        CHECK_EQ(d.anchorX(), cx);
        CHECK_EQ(d.anchorY(), cy);

        // From now on: the delta between consecutive client positions, and
        // no flip reported again.
        v = d.observe(true, cx, cy, 400, 900, 32000);
        CHECK(v.relative);
        CHECK(!v.changed);
        CHECK_EQ(v.deltaX, -30);
        CHECK_EQ(v.deltaY, -5);

        // Between a placement and the game's next frame the pointer is
        // legitimately off-centre: one miss changes nothing.
        v = d.observe(true, cx - 30, cy - 5, 390, 890, 40000);
        CHECK(v.relative);
        CHECK(!v.changed);
        // Back at the centre: the run of misses is forgotten.
        v = d.observe(true, cx, cy, 380, 880, 48000);
        CHECK(v.relative);

        // The game opens its menu (or quits): the pointer stays where the
        // deltas push it. Misses pile up, and after the grace period the
        // client's positions are placed again.
        v = d.observe(true, 700, 700, 370, 870, 56000);
        CHECK(v.relative);
        v = d.observe(true, 690, 690, 360, 860, 56000 + RecentreDetector::kMissToLeaveUs - 1);
        CHECK(v.relative);
        CHECK(!v.changed);
        v = d.observe(true, 680, 680, 350, 850, 56000 + RecentreDetector::kMissToLeaveUs);
        CHECK(!v.relative);
        CHECK(v.changed);
        CHECK(!d.relative());

        // And the desktop rule applies again: undisturbed placements stay
        // absolute.
        v = placeUndisturbed(d, 350, 850, 340, 840, 400000);
        CHECK(!v.relative);
        CHECK(!v.changed);
    }

    // ── A local hand on the host's mouse is not a game ──────────────────────
    {
        RecentreDetector d;
        d.observe(true, 0, 0, 100, 100, 0);
        // Found somewhere else every time, but never the same somewhere.
        RecentreDetector::Verdict v = d.observe(true, 900, 300, 110, 100, 8000);
        CHECK(!v.relative);
        v = d.observe(true, 950, 320, 120, 100, 16000);
        CHECK(!v.relative);
        v = d.observe(true, 1000, 340, 130, 100, 24000);
        CHECK(!v.relative);
        v = d.observe(true, 1050, 360, 140, 100, 32000);
        CHECK(!v.relative);
        CHECK(!d.relative());
        // Two returns to a spot, then the pointer found where it was put:
        // the count starts over.
        v = d.observe(true, 600, 600, 150, 100, 40000);
        v = d.observe(true, 600, 600, 160, 100, 48000);
        v = d.observe(true, 160, 100, 170, 100, 56000);
        CHECK(!v.relative);
        v = d.observe(true, 600, 600, 180, 100, 64000);
        CHECK(!v.relative);
        v = d.observe(true, 600, 600, 190, 100, 72000);
        CHECK(!v.relative);
    }

    // ── No pointer to read: nothing is ever decided ─────────────────────────
    {
        RecentreDetector d;
        d.observe(false, 0, 0, 100, 100, 0);
        for (int i = 1; i <= 10; ++i) {
            const RecentreDetector::Verdict v = d.observe(false, 0, 0, 100 + i, 100, i * 8000);
            CHECK(!v.relative);
            CHECK(!v.changed);
        }
    }

    // ── A re-entry across the picture is dropped, a mouse move is not ───────
    {
        // 2560 wide: a quarter is 640.
        CHECK(!RecentreDetector::isReentryJump(639, 0, 2560));
        CHECK(!RecentreDetector::isReentryJump(-639, 400, 2560));
        CHECK(RecentreDetector::isReentryJump(640, 0, 2560));
        CHECK(RecentreDetector::isReentryJump(0, -640, 2560));
        // No display known: anything non-zero is a jump, nothing turns.
        CHECK(RecentreDetector::isReentryJump(1, 0, 0));
        CHECK(!RecentreDetector::isReentryJump(0, 0, 0));
    }

    // ── One wording for every host ──────────────────────────────────────────
    {
        const std::string entered = RecentreDetector::enteredMessage(1280, 720);
        CHECK(entered.find("1280,720") != std::string::npos);
        CHECK(entered.find("[native] input:") == 0);
        CHECK(std::string(RecentreDetector::leftMessage()).find("[native] input:") == 0);
    }

    // ── reset(): a new display forgets the verdict and the last position ────
    {
        RecentreDetector d;
        d.observe(true, 0, 0, 400, 900, 0);
        d.observe(true, 1280, 720, 410, 900, 8000);
        d.observe(true, 1280, 720, 420, 900, 16000);
        RecentreDetector::Verdict v = d.observe(true, 1280, 720, 430, 900, 24000);
        CHECK(v.relative);
        d.reset();
        CHECK(!d.relative());
        v = d.observe(true, 1280, 720, 440, 900, 32000);
        CHECK(!v.relative);
        CHECK(!v.changed);
    }
}
