/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/macos/MacKeyMap.h"

#include <cstdio>
#include <map>
#include <set>

using namespace mw::native::input;

// The macOS key map, tested on whatever machine happens to run the suite.
//
// Same idea as the Linux table: literals rather than kVK_* macros so the map is
// checkable everywhere, and the properties no header can confirm are checked
// here — that the map is injective except where two virtual keys are MEANT to
// land on one Mac key, and that the awkward rows really are awkward in the way
// the code claims.

void run_mac_keymap_tests()
{
    SECTION("macOS key map — virtual key to kVK code");

    // ── The rows the code special-cases, spot-checked at both ends ──────────
    CHECK_EQ(macKeyCode(0x41), 0x00); // A   kVK_ANSI_A
    CHECK_EQ(macKeyCode(0x5A), 0x06); // Z   kVK_ANSI_Z
    CHECK_EQ(macKeyCode(0x51), 0x0C); // Q   kVK_ANSI_Q
    CHECK_EQ(macKeyCode(0x4D), 0x2E); // M   kVK_ANSI_M

    CHECK_EQ(macKeyCode(0x30), 0x1D); // 0   kVK_ANSI_0
    CHECK_EQ(macKeyCode(0x31), 0x12); // 1   kVK_ANSI_1
    CHECK_EQ(macKeyCode(0x39), 0x19); // 9   kVK_ANSI_9

    CHECK_EQ(macKeyCode(0x60), 0x52); // NUMPAD0  kVK_ANSI_Keypad0
    CHECK_EQ(macKeyCode(0x69), 0x5C); // NUMPAD9  kVK_ANSI_Keypad9

    CHECK_EQ(macKeyCode(0x70), 0x7A); // F1
    CHECK_EQ(macKeyCode(0x7B), 0x6F); // F12
    CHECK_EQ(macKeyCode(0x0D), 0x24); // Return
    CHECK_EQ(macKeyCode(0x20), 0x31); // Space
    CHECK_EQ(macKeyCode(0x08), 0x33); // Backspace → kVK_Delete
    CHECK_EQ(macKeyCode(0x2E), 0x75); // Delete → kVK_ForwardDelete

    // ── The modifiers, and where a PC keyboard's keys land on a Mac ─────────
    CHECK_EQ(macKeyCode(0x5B), 0x37); // Win → Command
    CHECK_EQ(macKeyCode(0x12), 0x3A); // Alt → Option
    CHECK_EQ(macKeyCode(0x11), 0x3B); // Ctrl → Control
    CHECK_EQ(macKeyCode(0xA1), 0x3C); // Right Shift
    CHECK(macIsModifier(0x10) && macIsModifier(0xA5) && macIsModifier(0x5C) && macIsModifier(0x14));
    CHECK(!macIsModifier(0x41) && !macIsModifier(0x0D) && !macIsModifier(0x20));

    // ── Keys that have no place on a Mac keyboard are dropped, not guessed ──
    CHECK_EQ(macKeyCode(0x5D), kMacNoKey); // VK_APPS
    CHECK_EQ(macKeyCode(0xAD), kMacNoKey); // VK_VOLUME_MUTE
    CHECK_EQ(macKeyCode(0xB0), kMacNoKey); // VK_MEDIA_NEXT_TRACK
    CHECK_EQ(macKeyCode(0x00), kMacNoKey);

    // ── Injective, except where two virtual keys MEAN the same Mac key ──────
    // Generic and left-hand modifiers (VK_SHIFT/VK_LSHIFT…) are one key;
    // Print Screen, Scroll Lock and Pause sit where F13–F15 do on an Apple
    // board; Num Lock and Clear share the keypad's Clear.
    const std::set<uint16_t> shared = {0x38, 0x3B, 0x3A, 0x69, 0x6B, 0x71, 0x47};
    std::map<uint16_t, int> owners;
    int mapped = 0;
    for (int vk = 1; vk < 0xFF; ++vk) {
        const uint16_t code = macKeyCode(vk);
        if (code == kMacNoKey) continue;
        mapped++;
        CHECK(code <= 0x7F); // ADB codes are 7-bit
        if (owners.count(code) && !shared.count(code)) {
            std::fprintf(stderr, "  [dup] kVK 0x%02X from VK 0x%02X and 0x%02X\n", code,
                         owners[code], vk);
            CHECK(false);
        }
        owners[code] = vk;
    }
    // Letters, digits, keypad, function row, typewriter and navigation keys:
    // a table that lost a row would show up here.
    CHECK(mapped >= 100);

    SECTION("macOS modifier flags — the client's mask settles the held ones");

    // The bug this exists for: the host believes Command is down (its key-up
    // was eaten by the Start menu), the client says nothing is held, and the
    // next Space would go out as ⌘Space and open Spotlight.
    CHECK_EQ(reconcileModifierFlags(kMacFlagCommand, 0x00), 0u);
    // A real chord is left alone: the client says meta, meta stays.
    CHECK_EQ(reconcileModifierFlags(kMacFlagCommand, 0x08), kMacFlagCommand);
    // …and one the host missed the press of is put back.
    CHECK_EQ(reconcileModifierFlags(0, 0x08), kMacFlagCommand);

    // Each bit of the mask, alone and together.
    CHECK_EQ(reconcileModifierFlags(0, 0x01), kMacFlagShift);
    CHECK_EQ(reconcileModifierFlags(0, 0x02), kMacFlagControl);
    CHECK_EQ(reconcileModifierFlags(0, 0x04), kMacFlagAlternate);
    CHECK_EQ(reconcileModifierFlags(0, 0x0F), kMacHeldFlags);
    CHECK_EQ(reconcileModifierFlags(kMacHeldFlags, 0x00), 0u);

    // Caps Lock is a LOCK, kept by syncLockKeys(): the mask never speaks for
    // it, in either direction.
    CHECK_EQ(reconcileModifierFlags(kMacFlagAlphaShift, 0x00), kMacFlagAlphaShift);
    CHECK_EQ(reconcileModifierFlags(kMacFlagAlphaShift | kMacFlagCommand, 0x00),
             kMacFlagAlphaShift);
    CHECK_EQ(reconcileModifierFlags(kMacFlagAlphaShift, 0x0F), kMacFlagAlphaShift | kMacHeldFlags);

    // Everything else the window server puts in there — Fn, numeric pad, the
    // device-dependent bits — is none of the client's business and survives.
    constexpr uint64_t kOther = 0x00800000 /*Fn*/ | 0x00200000 /*numeric pad*/ | 0x20000000;
    CHECK_EQ(reconcileModifierFlags(kOther | kMacFlagCommand, 0x00), kOther);
    CHECK_EQ(reconcileModifierFlags(kOther, 0x02), kOther | kMacFlagControl);

    // Applying the same mask twice changes nothing the first pass did not.
    for (uint8_t mask = 0; mask <= 0x0F; ++mask) {
        const uint64_t once = reconcileModifierFlags(kOther | kMacHeldFlags, mask);
        CHECK_EQ(reconcileModifierFlags(once, mask), once);
    }
}
