/*
 * MoonlightWeb — native capture & encoding engine: GPU load tool.
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

#include "ChipSong.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace {

constexpr double kTwoPi = 6.283185307179586;

// The YM2149's 16 volume steps: logarithmic, about 3 dB apart.
constexpr float kVolume[16] = {0.0f,    0.0106f, 0.0150f, 0.0222f, 0.0320f, 0.0466f,
                               0.0665f, 0.1039f, 0.1237f, 0.1986f, 0.2803f, 0.3548f,
                               0.4702f, 0.6030f, 0.7718f, 1.0f};

// ---- The score --------------------------------------------------------------
//
// A minor. The verse turns on Am | F | G | E — the E major, with its G#, is the
// pull back home; the bridge on Dm | G | C | E. Leads are written as a tracker
// would: one token per row (a 16th note), "-" holds, "." releases.

constexpr int kHold = -1;
constexpr int kRest = -2;

enum Lead
{
    NoLead = -1,
    A1,
    A2,
    A3,
    A4, // the theme
    Ap1,
    Ap2,
    Ap3,
    Ap4, // the theme, an octave of ambition higher
    B1,
    B2,
    B3,
    B4, // the bridge
    C1,
    C2,
    C3,
    C4, // the breakdown: long notes, vibrato
    BEnd,
    Final,
    LeadCount
};

const char* const kLeads[LeadCount] = {
    "A4 - - C5 - - E5 - A5 - - G5 - E5 - -",   "F5 - - E5 - - C5 - A4 - - C5 - D5 - -",
    "D5 - - B4 - - G4 - B4 - - D5 - G5 - -",   "G#5 - - - E5 - - - B4 - - - G#4 - B4 -",
    "A5 - - G5 - - E5 - C5 - - E5 - G5 - A5",  "C6 - - A5 - - F5 - A5 - - C6 - D6 - -",
    "B5 - - G5 - - D5 - G5 - - B5 - D6 - -",   "E6 - - - D6 - C6 - B5 - - - G#5 - - -",
    "D5 - F5 - A5 - F5 - D5 - F5 - A5 - C6 -", "B5 - - - A5 - G5 - - - D5 - G5 - A5 -",
    "C6 - - - G5 - E5 - - - C5 - E5 - G5 -",   "B5 - - - - - G#5 - - - E5 - G#5 - B5 -",
    "E5 - - - - - - - - - - - D5 - C5 -",      "C5 - - - - - - - - - - - A4 - C5 -",
    "D5 - - - - - - - - - - - B4 - D5 -",      "E5 - - - - - - - . . . . . . . .",
    "B5 - - - A5 - G#5 - - - E5 - - - . .",    "A5 - - - - - - - - - - - - - - .",
};

// Digidrum patterns, one row per character: kick, snare, closed hat, open hat.
enum Drums
{
    Intro,
    IntroFill,
    Full,
    Full16,
    Break,
    Roll,
    Fill,
    End,
    DrumCount
};

struct DrumPattern
{
    const char* kick;
    const char* snare;
    const char* closed;
    const char* open;
};

const DrumPattern kDrums[DrumCount] = {
    {"x...x...x...x...", "................", "................", "..x...x...x...x."},
    {"x...x...x...x...", "............x.xx", "................", "..x...x...x....."},
    {"x...x...x...x...", "....x.......x...", ".x.x.x.x.x.x.x.x", "..x...x...x...x."},
    {"x...x...x...x...", "....x.......x...", "xxxxxxxxxxxxxxxx", "..x...x...x...x."},
    {"................", "................", "x.x.x.x.x.x.x.x.", "..............x."},
    {"................", "x...x...x.x.xxxx", "x.x.x.x.........", "................"},
    {"x...x...x...x...", "....x...x.x.xxxx", ".x.x.x.x........", "................"},
    {"x...............", "x...............", "................", "x..............."},
};

enum BassMode
{
    BassOff,
    BassRiff,
    BassHold
};

struct Bar
{
    int root; // MIDI note of the chord's root, bass octave
    bool minor;
    Lead lead;
    BassMode bass;
    bool arp;
    Drums drums;
};

constexpr int Am = 45, F = 41, G = 43, E = 40, Dm = 38, C = 36;

const Bar kBars[] = {
    // Intro: the kick, the offbeat hats and the buzzer bass. The load calibrates.
    {Am, true, NoLead, BassRiff, false, Intro},
    {F, false, NoLead, BassRiff, false, Intro},
    {G, false, NoLead, BassRiff, false, Intro},
    {E, false, NoLead, BassRiff, false, IntroFill},
    // The arpeggios join.
    {Am, true, NoLead, BassRiff, true, Full},
    {F, false, NoLead, BassRiff, true, Full},
    {G, false, NoLead, BassRiff, true, Full},
    {E, false, NoLead, BassRiff, true, Full},
    // Theme.
    {Am, true, A1, BassRiff, true, Full},
    {F, false, A2, BassRiff, true, Full},
    {G, false, A3, BassRiff, true, Full},
    {E, false, A4, BassRiff, true, Full},
    // Theme, higher, 16th hats.
    {Am, true, Ap1, BassRiff, true, Full16},
    {F, false, Ap2, BassRiff, true, Full16},
    {G, false, Ap3, BassRiff, true, Full16},
    {E, false, Ap4, BassRiff, true, Full16},
    // Bridge.
    {Dm, true, B1, BassRiff, true, Full},
    {G, false, B2, BassRiff, true, Full},
    {C, false, B3, BassRiff, true, Full},
    {E, false, B4, BassRiff, true, Full},
    // Breakdown: no kick, no bass, then a snare roll.
    {Am, true, C1, BassOff, true, Break},
    {F, false, C2, BassOff, true, Break},
    {G, false, C3, BassOff, true, Break},
    {E, false, C4, BassOff, true, Roll},
    // Theme, higher, once more.
    {Am, true, Ap1, BassRiff, true, Full16},
    {F, false, Ap2, BassRiff, true, Full16},
    {G, false, Ap3, BassRiff, true, Full16},
    {E, false, Ap4, BassRiff, true, Full16},
    // Cadence and the final chord, just before the run's 60 s are up.
    {Dm, true, B1, BassRiff, true, Full},
    {E, false, BEnd, BassRiff, true, Fill},
    {Am, true, Final, BassHold, true, End},
};
constexpr int kBarCount = int(sizeof(kBars) / sizeof(kBars[0]));
constexpr int kSongRows = kBarCount * ChipSong::kRowsPerBar;

// The buzzer riff, in semitones over the root; -1 is a rest.
constexpr int kBassRiff[16] = {0, -1, 12, 0, -1, 0, 12, -1, 0, -1, 12, 0, 7, -1, 12, 0};

int parseNote(const std::string& token)
{
    if (token == "-") return kHold;
    if (token == ".") return kRest;
    static const int letters[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    int semitone = letters[token[0] - 'A'];
    size_t i = 1;
    if (token[i] == '#') {
        ++semitone;
        ++i;
    }
    return 12 * (token[i] - '0' + 1) + semitone;
}

using LeadRows = std::array<int, 16>;

const std::array<LeadRows, LeadCount>& leadRows()
{
    static const std::array<LeadRows, LeadCount> rows = [] {
        std::array<LeadRows, LeadCount> out{};
        for (int l = 0; l < LeadCount; ++l) {
            std::string text = kLeads[l];
            size_t pos = 0;
            for (int step = 0; step < 16; ++step) {
                const size_t end = text.find(' ', pos);
                out[l][step] = parseNote(text.substr(pos, end - pos));
                pos = end == std::string::npos ? text.size() : end + 1;
            }
        }
        return out;
    }();
    return rows;
}

bool hit(const char* pattern, int step)
{
    return pattern[step] == 'x';
}

double noteHz(double note)
{
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

// Band-limited edges (PolyBLEP): a naive square at 48 kHz aliases into a hiss
// that would hide the clicks this tune is meant to reveal.
double polyBlep(double t, double dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

double pulse(double phase, double dt, double duty)
{
    double v = phase < duty ? 1.0 : -1.0;
    v += polyBlep(phase, dt);
    v -= polyBlep(std::fmod(phase + 1.0 - duty, 1.0), dt);
    return v;
}

double sawDown(double phase, double dt)
{
    return 1.0 - 2.0 * phase + polyBlep(phase, dt);
}

void advance(double& phase, double dt)
{
    phase += dt;
    if (phase >= 1.0) phase -= 1.0;
}

} // namespace

ChipSong::ChipSong()
    : m_echo(size_t(kRowFrames) * 3, 0.0f) // a dotted eighth
{
    leadRows();
}

qint64 ChipSong::songFrames()
{
    return qint64(kSongRows) * kRowFrames;
}

float ChipSong::kickPulse(qint64 frame)
{
    if (frame < 0) return 0.0f;
    const qint64 row = frame / kRowFrames;
    for (qint64 r = row; r >= 0 && r > row - 8; --r) {
        const int songRow = int(r % kSongRows);
        const Bar& bar = kBars[songRow / kRowsPerBar];
        if (!hit(kDrums[bar.drums].kick, songRow % kRowsPerBar)) continue;
        const double seconds = double(frame - r * kRowFrames) / kRate;
        return float(std::exp(-seconds * 7.0));
    }
    return 0.0f;
}

void ChipSong::tick()
{
    const int songTick = int(m_tick % (qint64(kSongRows) * kTicksPerRow));
    const int row = songTick / kTicksPerRow;
    const Bar& bar = kBars[row / kRowsPerBar];
    const int step = row % kRowsPerBar;

    if (songTick % kTicksPerRow == 0) {
        if (bar.bass == BassRiff && kBassRiff[step] >= 0) {
            m_bass.freq = noteHz(bar.root + kBassRiff[step]);
            m_bassGate = 4; // of 6 ticks: the gap is the pump
        } else if (bar.bass == BassHold && step == 0) {
            m_bass.freq = noteHz(bar.root);
            m_bassGate = 90;
        }
        if (bar.arp && step % 2 == 0) {
            m_arpRoot = bar.root + 24;
            m_arpMinor = bar.minor;
            m_arpAge = 0;
        }
        const int token = bar.lead == NoLead ? kRest : leadRows()[bar.lead][step];
        if (token >= 0) {
            m_leadNote = token;
            m_leadAge = 0;
            m_leadHeld = true;
        } else if (token == kRest) {
            m_leadHeld = false;
        }
        const DrumPattern& d = kDrums[bar.drums];
        if (hit(d.kick, step)) {
            m_kickT = 0.0;
            m_kickPhase = 0.0;
        }
        if (hit(d.snare, step)) {
            m_snareT = 0.0;
            m_snarePhase = 0.0;
        }
        if (hit(d.closed, step)) {
            m_hatT = 0.0;
            m_hatGain = step % 4 == 2 ? 1.0f : 0.6f;
        }
        if (hit(d.open, step)) m_openT = 0.0;
    }

    // Bass: full volume while the gate is open; the last chord fades.
    if (m_bassGate > 0) {
        m_bass.volume = bar.bass == BassHold ? std::max(0, 15 - (90 - m_bassGate) / 6) : 15;
        --m_bassGate;
    } else {
        m_bass.volume = 0;
    }

    // Arpeggio: root, third, fifth, a different note every 50 Hz tick — the
    // chord a three-voice chip could not otherwise play.
    if (m_arpAge < 12) {
        static const int minor[3] = {0, 3, 7};
        static const int major[3] = {0, 4, 7};
        const int tone = (m_arpMinor ? minor : major)[m_tick % 3];
        m_arp.freq = noteHz(m_arpRoot + tone);
        m_arp.volume = 11 - m_arpAge / 2;
        ++m_arpAge;
    } else {
        m_arp.volume = 0;
    }

    // Lead: an octave chirp on the attack tick, a short decay, then vibrato
    // once the note has lasted; released notes fall off in a few ticks.
    if (m_leadNote >= 0) {
        double pitch = m_leadNote;
        if (m_leadHeld) {
            static const int attack[4] = {15, 14, 13, 12};
            m_lead.volume = m_leadAge < 4 ? attack[m_leadAge] : 12;
            if (m_leadAge == 0) pitch += 12.0;
            if (m_leadAge >= 10) {
                const double depth = std::min(0.3, (m_leadAge - 10) * 0.03);
                pitch += depth * std::sin(kTwoPi * 6.0 * (m_leadAge - 10) / 50.0);
            }
        } else {
            m_lead.volume = std::max(0, m_lead.volume - 2);
        }
        m_lead.freq = noteHz(pitch);
        m_lead.duty = 0.34 + 0.16 * std::sin(kTwoPi * double(m_tick) / 150.0);
        ++m_leadAge;
    }

    ++m_tick;
}

float ChipSong::drums()
{
    // A 17-bit LFSR, the YM's noise generator, stepped every sample for the
    // hats and held for three samples (16 kHz) for the snare.
    const quint32 bit = (m_lfsr ^ (m_lfsr >> 3)) & 1u;
    m_lfsr = (m_lfsr >> 1) | (bit << 16);
    const float white = (m_lfsr & 1u) ? 1.0f : -1.0f;
    const float bright = white - m_noisePrev; // first difference: a crude high-pass
    m_noisePrev = white;
    m_noiseClock += 16000.0 / kRate;
    if (m_noiseClock >= 1.0) {
        m_noiseClock -= 1.0;
        m_noise = white;
    }

    const double dt = 1.0 / kRate;
    double v = 0.0;
    if (m_kickT >= 0.0) {
        const double f = 46.0 + 130.0 * std::exp(-m_kickT * 30.0);
        advance(m_kickPhase, f * dt);
        v += 0.7 * std::sin(kTwoPi * m_kickPhase) * std::exp(-m_kickT * 6.5);
        if (m_kickT < 0.003) v += 0.2 * white * (1.0 - m_kickT / 0.003);
        m_kickT = m_kickT + dt > 0.5 ? -1.0 : m_kickT + dt;
    }
    if (m_snareT >= 0.0) {
        advance(m_snarePhase, (170.0 + 30.0 * std::exp(-m_snareT * 40.0)) * dt);
        v += 0.35 * std::sin(kTwoPi * m_snarePhase) * std::exp(-m_snareT * 25.0);
        v += 0.5 * m_noise * std::exp(-m_snareT * 14.0);
        m_snareT = m_snareT + dt > 0.35 ? -1.0 : m_snareT + dt;
    }
    if (m_hatT >= 0.0) {
        v += 0.09 * m_hatGain * bright * std::exp(-m_hatT * 80.0);
        m_hatT = m_hatT + dt > 0.08 ? -1.0 : m_hatT + dt;
    }
    if (m_openT >= 0.0) {
        v += 0.07 * bright * std::exp(-m_openT * 13.0);
        m_openT = m_openT + dt > 0.4 ? -1.0 : m_openT + dt;
    }
    return float(v);
}

void ChipSong::render(qint16* out, int frames)
{
    const int blip = m_blip.exchange(0);
    if (blip != 0) {
        m_blipT = 0.0;
        m_blipPhase = 0.0;
        m_blipDir = blip;
    }
    const float lowPass = float(std::exp(-kTwoPi * 12000.0 / kRate));

    for (int i = 0; i < frames; ++i) {
        if (m_tickFrame == 0) tick();
        if (++m_tickFrame == kTickFrames) m_tickFrame = 0;

        // Channel A, the buzzer: a square gated by a slightly detuned
        // sawtooth, whose drift makes the bass sweep like a filter.
        double bass = 0.0;
        const double bassDt = m_bass.freq / kRate;
        const double envDt = m_bass.freq * 1.004 / kRate;
        if (m_bass.volume > 0) {
            const double ramp = 0.5 + 0.5 * sawDown(m_bassEnvPhase, envDt);
            bass = pulse(m_bass.phase, bassDt, 0.5) * ramp * kVolume[m_bass.volume];
        }
        advance(m_bass.phase, bassDt);
        advance(m_bassEnvPhase, envDt);

        double arp = 0.0;
        const double arpDt = m_arp.freq / kRate;
        if (m_arp.volume > 0) arp = pulse(m_arp.phase, arpDt, 0.5) * kVolume[m_arp.volume];
        advance(m_arp.phase, arpDt);

        double lead = 0.0;
        const double leadDt = m_lead.freq / kRate;
        if (m_lead.volume > 0)
            lead = pulse(m_lead.phase, leadDt, m_lead.duty) * kVolume[m_lead.volume];
        advance(m_lead.phase, leadDt);

        double blipOut = 0.0;
        if (m_blipT >= 0.0) {
            const double sweep = std::pow(3.0, m_blipT / 0.08);
            const double f = m_blipDir > 0 ? 600.0 * sweep : 1800.0 / sweep;
            blipOut = 0.5 * pulse(m_blipPhase, f / kRate, 0.5) * (1.0 - m_blipT / 0.09);
            advance(m_blipPhase, f / kRate);
            m_blipT = m_blipT + 1.0 / kRate > 0.09 ? -1.0 : m_blipT + 1.0 / kRate;
        }

        // The lead's echo, bounced to the right.
        const float leadMix = float(lead * 0.32);
        const float echo = m_echo[m_echoPos];
        m_echo[m_echoPos] = leadMix + echo * 0.35f;
        if (++m_echoPos == m_echo.size()) m_echoPos = 0;

        const float center = float(bass * 0.34) + drums() + float(blipOut * 0.3);
        const float arpMix = float(arp * 0.22);
        const float mix[2] = {center + 0.35f * arpMix + 0.65f * leadMix + 0.20f * echo,
                              center + 0.65f * arpMix + 0.35f * leadMix + 0.45f * echo};
        for (int c = 0; c < 2; ++c) {
            float x = mix[c] * 0.8f;
            x = x / (1.0f + std::fabs(x) * 0.5f); // soft clip, never a hard edge
            m_lp[c] = x + lowPass * (m_lp[c] - x);
            const float y = m_lp[c] - m_dcIn[c] + 0.9995f * m_dcOut[c];
            m_dcIn[c] = m_lp[c];
            m_dcOut[c] = y;
            out[2 * i + c] = qint16(std::lround(std::clamp(y, -1.0f, 1.0f) * 32000.0f));
        }
    }
}
