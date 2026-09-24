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

#include "mw/native/NativeHost.h"
#include "mw/native/VirtualDisplay.h"
#include "native_test_framework.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

// The in-process virtual display seam, on every platform.
//
// What is checked is the contract the server relies on, not the OS: a process
// starts with no display; a refused request leaves that so and says why;
// destroy() with nothing held is harmless; the honest stub says "not
// supported" with a reason where there is no backend. Creating a real one is
// the Mac bench's job — it needs a window server session no CI runner has.

using namespace mw::native;

void run_virtual_display_tests()
{
    SECTION("VirtualDisplay — a process starts without one, and destroy() of nothing is nothing");
    {
        CHECK(!vdisplay::isActive());
        CHECK(!vdisplay::isOnline());
        CHECK_EQ(vdisplay::displayId(), 0u);
        vdisplay::destroy();
        CHECK(!vdisplay::isActive());
    }

    SECTION("VirtualDisplay — a refused request explains itself and holds nothing");
    {
        vdisplay::Spec bad;
        bad.width = 0;
        std::string error;
        CHECK(!vdisplay::create(bad, &error));
        CHECK(!error.empty());
        CHECK(!vdisplay::isActive());
        CHECK(!vdisplay::create(bad, nullptr)); // the error pointer is optional
    }

    SECTION("VirtualDisplay — unsupported here means a reason, and every create refused");
    {
        if (vdisplay::isSupported()) {
            CHECK(vdisplay::unsupportedReason().empty());
        } else {
            CHECK(!vdisplay::unsupportedReason().empty());
            vdisplay::Spec spec;
            std::string error;
            CHECK(!vdisplay::create(spec, &error));
            CHECK_EQ(error, vdisplay::unsupportedReason());
        }
    }

    SECTION("VirtualDisplay — the default name reads as virtual to the probe");
    {
        // MacProbe classifies by name (nameLooksVirtual): a rename that dropped
        // the word would make our own display look like an external monitor.
        vdisplay::Spec spec;
        CHECK(spec.name.find("Virtual") != std::string::npos);
    }

#if defined(__APPLE__)
    // A real display, on the Mac bench only (MW_TEST_VIRTUAL_DISPLAY=1): it
    // needs the window server of a logged-in session. SDR first, then HDR —
    // what the probe says about each is what the Selector will act on.
    SECTION("VirtualDisplay (macOS) — an HDR display is one the probe calls HDR");
    if (!std::getenv("MW_TEST_VIRTUAL_DISPLAY")) {
        std::fprintf(stderr, "  skipped: MW_TEST_VIRTUAL_DISPLAY not set\n");
    } else {
        // The server is an application with a window-server connection; this
        // binary is not, and without one the display never gets its mode or
        // its name (it lists as "Display — 0×0"). NSApp, made the way AppKit
        // would, from C++.
        using Msg = void* (*)(void*, SEL);
        reinterpret_cast<Msg>(objc_msgSend)(reinterpret_cast<void*>(objc_getClass("NSApplication")),
                                            sel_registerName("sharedApplication"));
        for (const bool hdr : {false, true}) {
            vdisplay::Spec spec;
            spec.width = 1920;
            spec.height = 1080;
            spec.refreshHz = 60;
            spec.hdr = hdr;
            std::string error;
            const bool made = vdisplay::create(spec, &error);
            if (!made) std::fprintf(stderr, "  create failed: %s\n", error.c_str());
            CHECK(made);
            const DisplayInfo* ours = nullptr;
            Capabilities caps;
            for (int i = 0; made && i < 50 && !ours; ++i) {
                // The run loop turns: the display comes online on the main
                // queue (its descriptor's), which a sleep would starve.
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                caps = NativeHost::probe();
                for (const DisplayInfo& d : caps.displays)
                    if (d.detail.find("Virtual") != std::string::npos) ours = &d;
            }
            if (!ours)
                for (const DisplayInfo& d : caps.displays)
                    std::fprintf(stderr, "  listed: %s (%s)\n", d.label.c_str(), d.detail.c_str());
            CHECK(ours != nullptr);
            if (ours) {
                std::fprintf(stderr, "  %s display: %s %dx%d, hdrActive %d\n", hdr ? "HDR" : "SDR",
                             ours->label.c_str(), ours->width, ours->height, ours->hdrActive);
                CHECK_EQ(ours->hdrActive, hdr);
            }
            vdisplay::destroy();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        }
    }
#endif
}
