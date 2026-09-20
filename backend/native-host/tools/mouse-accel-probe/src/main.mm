/*
 * MoonlightWeb — mouse acceleration probe (lab tool, never shipped).
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

// What curve does THIS Mac apply to THIS mouse?
//
// MacPointerAccel reproduces the shape of Apple's pointer acceleration, since
// no API runs the real table for an injected event. The shape needs three
// numbers, and this measures them instead of leaving them to taste.
//
// ── How it works ───────────────────────────────────────────────────────────
//
// A real mouse event carries both halves of the answer: kCGMouseEventDeltaX/Y
// is the ACCELERATED movement (the points the pointer actually travelled) and
// fields 170/171 are the mouse's own counts, before the driver touched them.
// The ratio of the two IS the gain, at the speed the counts imply. So the tool
// listens — it injects nothing, changes nothing — while a hand moves the mouse
// over the whole range it would in a game, buckets the samples by speed, and
// fits the curve to them.
//
// ── Using it ───────────────────────────────────────────────────────────────
//
//   mouse-accel-probe [seconds]            (default 30)
//
// Move the mouse for the whole run: slow careful aim, medium tracking, fast
// flicks, several of each. The tool ends with the MW_MOUSE_ACCEL_* line to put
// in the host's environment.
//
// Needs Accessibility (or Input Monitoring) for the event tap — macOS will say
// so, and the tool says which switch if the tap is refused.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/// The mouse's own counts, before the driver's curve. Spelled as numbers
/// rather than by name so an older SDK still builds the tool; an unknown
/// field simply reads 0, which the run reports rather than hides.
constexpr CGEventField kUnacceleratedX = static_cast<CGEventField>(170);
constexpr CGEventField kUnacceleratedY = static_cast<CGEventField>(171);

/// The factory tracking speed, the unit MacPointerAccel normalises by.
constexpr double kFactoryScaling = 0.6875;

/// Speed buckets, in inches per second of desk travel. Fine where aiming
/// happens, coarse out where only flicks live.
const double kBucketEdges[] = {0.5, 1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 48, 64, 85, 120, 1e9};
constexpr int kBucketCount = static_cast<int>(sizeof(kBucketEdges) / sizeof(kBucketEdges[0]));

struct Bucket
{
    double rawSum = 0;   ///< counts seen in this bucket
    double pointSum = 0; ///< points they became
    double speedSum = 0; ///< speed weighted by counts, for the bucket's centre
    uint64_t samples = 0;
};

struct Probe
{
    Bucket buckets[kBucketCount];
    uint64_t events = 0;
    uint64_t withRaw = 0; ///< events that carried the unaccelerated fields
    double cpi = 400.0;
    uint64_t lastTimestampNs = 0;
};

Probe g_probe;

/// The tracking-speed slider, read the same way the host reads it.
bool readMouseScaling(double& scaling)
{
    io_service_t service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
    if (!service) return false;
    io_connect_t connect = 0;
    bool ok = false;
    if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) ==
        KERN_SUCCESS) {
        double value = 0;
        // Deprecated since 10.12, and the same call the host makes — the tool
        // has to read the slider exactly as MacPointerAccel does or the number
        // it prints would be fitted against a different baseline.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const kern_return_t asked =
            IOHIDGetAccelerationWithKey(connect, CFSTR(kIOHIDMouseAccelerationType), &value);
#pragma clang diagnostic pop
        if (asked == KERN_SUCCESS) {
            scaling = value;
            ok = true;
        }
        IOServiceClose(connect);
    }
    IOObjectRelease(service);
    return ok;
}

int bucketFor(double ips)
{
    for (int i = 0; i < kBucketCount; ++i)
        if (ips < kBucketEdges[i]) return i;
    return kBucketCount - 1;
}

CGEventRef onEvent(CGEventTapProxy, CGEventType type, CGEventRef event, void*)
{
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
        return event;

    const double accelX =
        static_cast<double>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
    const double accelY =
        static_cast<double>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
    const double rawX = static_cast<double>(CGEventGetIntegerValueField(event, kUnacceleratedX));
    const double rawY = static_cast<double>(CGEventGetIntegerValueField(event, kUnacceleratedY));

    const uint64_t nowNs = CGEventGetTimestamp(event);
    const uint64_t sinceNs = g_probe.lastTimestampNs > 0 && nowNs > g_probe.lastTimestampNs
                                 ? nowNs - g_probe.lastTimestampNs
                                 : 0;
    g_probe.lastTimestampNs = nowNs;

    ++g_probe.events;
    const double raw = std::sqrt(rawX * rawX + rawY * rawY);
    const double points = std::sqrt(accelX * accelX + accelY * accelY);
    if (raw <= 0) return event;
    ++g_probe.withRaw;

    // Same floor as the host's: no mouse reports faster than 1 kHz, and a
    // timestamp pair that says otherwise is the clock, not the hand.
    const double intervalS = std::max(sinceNs > 0 ? sinceNs / 1e9 : 0.001, 0.001);
    const double ips = (raw / intervalS) / g_probe.cpi;

    Bucket& b = g_probe.buckets[bucketFor(ips)];
    b.rawSum += raw;
    b.pointSum += points;
    b.speedSum += ips * raw;
    ++b.samples;
    return event;
}

/// Sum of squared error, in log-gain, between the measured buckets and a curve.
/// Log because a factor-of-two miss matters as much at gain 1 as at gain 6.
double curveError(double scale, double gainMax, double halfIps, double power)
{
    double error = 0;
    int used = 0;
    for (const Bucket& b : g_probe.buckets) {
        if (b.samples < 8 || b.rawSum <= 0) continue;
        const double measured = b.pointSum / b.rawSum;
        const double ips = b.speedSum / b.rawSum;
        const double v = std::pow(ips, power);
        const double half = std::pow(halfIps, power);
        const double modelled = scale * (1.0 + (gainMax - 1.0) * v / (v + half));
        if (measured <= 0 || modelled <= 0) continue;
        const double d = std::log(measured) - std::log(modelled);
        // Weighted by how much of the movement landed here: a bucket with one
        // twitch in it should not pull the curve as hard as a whole stroke.
        error += d * d * b.rawSum;
        ++used;
    }
    return used > 0 ? error : 1e30;
}

void report(double scaling)
{
    const double scale = scaling > 0 ? scaling / kFactoryScaling : 1.0;

    std::printf("\n");
    if (g_probe.withRaw == 0) {
        std::printf("No event carried the unaccelerated fields (170/171): %llu mouse event(s)\n"
                    "were seen but none said what the mouse itself reported, so no gain can be\n"
                    "measured on this macOS. Fall back to tuning MW_MOUSE_ACCEL_* by feel.\n",
                    static_cast<unsigned long long>(g_probe.events));
        return;
    }

    std::printf("%llu mouse event(s), %llu with the mouse's own counts.\n",
                static_cast<unsigned long long>(g_probe.events),
                static_cast<unsigned long long>(g_probe.withRaw));
    std::printf("Tracking speed %.4f (x%.2f of the factory setting), counts read at %.0f CPI.\n\n",
                scaling, scale, g_probe.cpi);

    std::printf("  desk speed      counts    points    measured gain\n");
    double slowGain = 0;
    double slowWeight = 0;
    double fastGain = 0;
    for (int i = 0; i < kBucketCount; ++i) {
        const Bucket& b = g_probe.buckets[i];
        if (b.samples < 8 || b.rawSum <= 0) continue;
        const double ips = b.speedSum / b.rawSum;
        const double gain = b.pointSum / b.rawSum;
        std::printf("  %7.1f in/s  %9.0f %9.0f    x%.2f\n", ips, b.rawSum, b.pointSum, gain);
        if (ips < 3.0) {
            slowGain += gain * b.rawSum;
            slowWeight += b.rawSum;
        }
        fastGain = std::max(fastGain, gain);
    }
    if (slowWeight <= 0 || fastGain <= 0) {
        std::printf("\nNot enough movement to fit a curve. Run it again and cover the whole\n"
                    "range: slow aim, medium tracking, and fast flicks.\n");
        return;
    }
    const double measuredSlow = slowGain / slowWeight;
    std::printf("\nGain at a standstill: x%.2f (the host reads x%.2f from the slider)\n",
                measuredSlow, scale);

    // Three numbers, found by search rather than by algebra: the error surface
    // is small, the grid is cheap, and a closed form would not survive the
    // first bucket a hand failed to fill.
    double bestMax = 0, bestHalf = 0, bestPower = 0, bestError = 1e30;
    for (double gainMax = 1.5; gainMax <= 20.001; gainMax += 0.25)
        for (double halfIps = 2.0; halfIps <= 60.001; halfIps *= 1.12)
            for (double power = 0.8; power <= 4.001; power += 0.1) {
                const double error = curveError(measuredSlow, gainMax, halfIps, power);
                if (error < bestError) {
                    bestError = error;
                    bestMax = gainMax;
                    bestHalf = halfIps;
                    bestPower = power;
                }
            }
    if (bestError >= 1e29) {
        std::printf("No bucket had enough samples to fit. Move the mouse more, and slower and\n"
                    "faster than felt natural.\n");
        return;
    }

    std::printf("\nFitted curve — put this in the host's environment:\n\n");
    // The gain override is only needed when the measured standstill gain and
    // the slider disagree; when they agree the host reads the slider itself,
    // which keeps following the user if they move it.
    if (std::fabs(measuredSlow - scale) / scale > 0.15)
        std::printf("  MW_MOUSE_ACCEL_GAIN=%.2f \\\n", measuredSlow);
    std::printf("  MW_MOUSE_ACCEL_MAX=%.2f MW_MOUSE_ACCEL_HALF=%.1f MW_MOUSE_ACCEL_POW=%.1f\n\n",
                bestMax, bestHalf, bestPower);
    std::printf("If the mouse used here is not %.0f CPI, re-run with its own value:\n"
                "  mouse-accel-probe <seconds> <cpi>   — and pass the same MW_MOUSE_ACCEL_CPI.\n",
                g_probe.cpi);
}

} // namespace

int main(int argc, char** argv)
{
    double seconds = 30;
    if (argc > 1) seconds = std::max(2.0, std::strtod(argv[1], nullptr));
    if (argc > 2) {
        const double cpi = std::strtod(argv[2], nullptr);
        if (cpi >= 100 && cpi <= 32000) g_probe.cpi = cpi;
    }

    double scaling = 0;
    if (!readMouseScaling(scaling))
        std::printf("Warning: the tracking speed could not be read from IOHIDSystem.\n");
    if (scaling < 0)
        std::printf("Note: this Mac has pointer acceleration turned off, so the gain should\n"
                    "measure flat at x1 — which is what the host will do too.\n");

    CFMachPortRef tap = CGEventTapCreate(
        kCGHIDEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly,
        CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged) |
            CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged),
        onEvent, nullptr);
    if (!tap) {
        std::fprintf(stderr,
                     "Could not listen to mouse events. macOS wants this program allowed in\n"
                     "System Settings > Privacy & Security > Input Monitoring (Accessibility\n"
                     "also works), and the switch has to be on for the binary being run.\n");
        return 1;
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);

    std::printf("Listening for %.0f s. Move the mouse the way a game would: slow aim, steady\n"
                "tracking, and fast flicks — several of each, and nothing else running that\n"
                "moves the pointer.\n",
                seconds);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);

    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(tap);

    report(scaling);
    return 0;
}
