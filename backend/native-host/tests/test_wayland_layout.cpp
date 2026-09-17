/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "native_test_framework.h"

#include "input/linux/WaylandLayout.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mw::native::input;

// The absolute pointer's rectangles from a Wayland compositor's layout (issue
// #18). KMS cannot say where a Wayland output sits — every CRTC scans out from
// (0, 0) there — so the compositor's logical layout is what the mapping has to
// be built from, and this is the choice: the captured connector's rectangle,
// and the union of every output as the desktop.
//
// Pure arithmetic in the header, so every platform's run covers it; the
// Wayland socket itself is exercised only where there is one (below).

void run_wayland_layout_tests()
{
    SECTION("Wayland layout — the captured connector's rectangle and the desktop around it");

    int dl = -1, dt = -1, dr = -1, db = -1;
    int kl = -1, kt = -1, kr = -1, kb = -1;

    // Two monitors side by side, the stream on the second one. This is the
    // report: under KMS both would sit at (0, 0) and the pointer would move on
    // the first.
    std::vector<WaylandOutput> two = {{"DP-1", 0, 0, 2560, 1440}, {"DP-2", 2560, 0, 1920, 1080}};
    CHECK(pickWaylandRects(two, "DP-2", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, 2560);
    CHECK_EQ(dt, 0);
    CHECK_EQ(dr, 4480);
    CHECK_EQ(db, 1080);
    CHECK_EQ(kl, 0);
    CHECK_EQ(kt, 0);
    CHECK_EQ(kr, 4480);
    CHECK_EQ(kb, 1440);

    // The first one: its own rectangle, the same desktop.
    CHECK(pickWaylandRects(two, "DP-1", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, 0);
    CHECK_EQ(dr, 2560);
    CHECK_EQ(db, 1440);
    CHECK_EQ(kr, 4480);
    CHECK_EQ(kb, 1440);

    // A scaled monitor reports its LOGICAL size — 1920x1080 at 1.5 is 1280x720
    // — and a monitor placed to the left has negative coordinates. Both are
    // ordinary here; the desktop's origin follows the leftmost one.
    std::vector<WaylandOutput> scaled = {{"HDMI-A-1", -1280, 0, 1280, 720},
                                         {"DP-3", 0, 0, 3840, 2160}};
    CHECK(pickWaylandRects(scaled, "HDMI-A-1", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, -1280);
    CHECK_EQ(dr, 0);
    CHECK_EQ(kl, -1280);
    CHECK_EQ(kr, 3840);
    CHECK_EQ(kb, 2160);

    // The connector, spelled by a compositor that lowercases — matched anyway.
    CHECK(pickWaylandRects(two, "dp-2", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, 2560);

    // Mutter's spelling: the kernel's HDMI-A-1 is GNOME's HDMI-1 (seen on the
    // UM790Pro bench, GNOME 42). Either side may carry either form.
    CHECK_EQ(canonicalConnectorName("HDMI-A-1"), std::string("hdmi-1"));
    CHECK_EQ(canonicalConnectorName("HDMI-1"), std::string("hdmi-1"));
    CHECK_EQ(canonicalConnectorName("DP-2"), std::string("dp-2"));
    CHECK_EQ(canonicalConnectorName("eDP-1"), std::string("edp-1"));
    std::vector<WaylandOutput> gnome = {{"HDMI-1", 0, 0, 1920, 1080},
                                        {"DP-1", 1920, 0, 1920, 1080}};
    CHECK(pickWaylandRects(gnome, "HDMI-A-1", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, 0);
    CHECK_EQ(dr, 1920);
    CHECK_EQ(kr, 3840);
    std::vector<WaylandOutput> kernelNamed = {{"HDMI-A-2", 0, 0, 1920, 1080}};
    CHECK(pickWaylandRects(kernelNamed, "HDMI-2", dl, dt, dr, db, kl, kt, kr, kb));

    // Not among the outputs: nothing chosen, nothing touched, and the caller
    // keeps what KMS said. The portal route's "portal" is the everyday case.
    dl = dt = dr = db = kl = kt = kr = kb = -7;
    CHECK(!pickWaylandRects(two, "portal", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK(!pickWaylandRects(two, "", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dl, -7);
    CHECK_EQ(kr, -7);

    // An output with no size yet (a compositor mid-configuration) is skipped
    // for the desktop and cannot be the display.
    std::vector<WaylandOutput> half = {{"DP-1", 0, 0, 1920, 1080}, {"DP-2", 1920, 0, 0, 0}};
    CHECK(!pickWaylandRects(half, "DP-2", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK(pickWaylandRects(half, "DP-1", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(kr, 1920);

    // One monitor: the display IS the desktop, which is what the mapping
    // reduces to on a single-screen host.
    std::vector<WaylandOutput> one = {{"eDP-1", 0, 0, 1920, 1200}};
    CHECK(pickWaylandRects(one, "eDP-1", dl, dt, dr, db, kl, kt, kr, kb));
    CHECK_EQ(dr, kr);
    CHECK_EQ(db, kb);

    // The portal route names no connector but says where its monitor sits;
    // the output at exactly that rectangle is the monitor, and the desktop is
    // the union around it whatever the names.
    std::string name;
    CHECK(findWaylandOutputAt(two, 2560, 0, 4480, 1080, name));
    CHECK_EQ(name, std::string("DP-2"));
    CHECK(!findWaylandOutputAt(two, 0, 0, 1920, 1080, name)); // a size, not a place
    std::vector<WaylandOutput> twins = {{"DP-1", 0, 0, 1920, 1080}, {"DP-2", 0, 0, 1920, 1080}};
    CHECK(!findWaylandOutputAt(twins, 0, 0, 1920, 1080, name)); // mirrored: ambiguous
    CHECK(waylandDesktopUnion(scaled, kl, kt, kr, kb));
    CHECK_EQ(kl, -1280);
    CHECK_EQ(kr, 3840);
    CHECK(!waylandDesktopUnion({}, kl, kt, kr, kb));
    CHECK(!waylandDesktopUnion({{"DP-1", 0, 0, 0, 0}}, kl, kt, kr, kb));

#if defined(__linux__)
    // The socket, where there is one: the run on a Wayland desktop shows the
    // compositor's own answer and proves the hand-written protocol tables
    // against a real libwayland. Anywhere else — a headless runner, X11 — the
    // read says why and that is fine.
    SECTION("Wayland layout — the compositor's answer, on a host that has one");
    std::vector<WaylandOutput> live;
    std::string socketUsed;
    std::string why;
    if (WaylandLayout::read(live, socketUsed, why)) {
        std::printf("    Wayland layout via %s:\n", socketUsed.c_str());
        for (const WaylandOutput& out : live)
            std::printf("      %s %d,%d %dx%d\n", out.name.c_str(), out.x, out.y, out.width,
                        out.height);
        CHECK(!live.empty());
        for (const WaylandOutput& out : live) {
            CHECK(out.width > 0);
            CHECK(out.height > 0);
        }
    } else {
        std::printf("    no Wayland layout here: %s\n", why.c_str());
    }
#endif
}
