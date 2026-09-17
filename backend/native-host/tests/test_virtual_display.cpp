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

#include "mw/native/VirtualDisplay.h"
#include "native_test_framework.h"

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
}
