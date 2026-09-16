/*
 * MoonlightWeb — self-hosted web client for Moonlight game streaming.
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

#pragma once

#include <cstdint>
#include <mutex>

// The moments the link stopped carrying anything, counted for the viewer.
//
// ── Why the stats needed it ─────────────────────────────────────────────────
//
// Measured on a corporate Wi-Fi (16/09/2026): every 20 to 30 s the link froze
// for 0.3 to 1.2 s in both directions, and the stats card still read a 20 ms
// latency. Nothing it showed could see a freeze: NETWORK is a ping every two
// seconds averaged over five, and FRAMES LOST counts frameId gaps — frames the
// host dropped behind a stuck buffer never got an id. Only the host log said
// it: "link not draining" and "Input link silent", at the same second.
//
// Those two lines are the two directions of one freeze, and this is where they
// meet:
//  - down: the video channel's buffer stayed backed up past SendBacklog's
//    tolerance (the relay sheds a frame and notes the backlog's span);
//  - up: the client went silent long enough for the input watchdog to fire,
//    measured when its next message lands.
// An episode that starts within kMergeMs of the previous one's end is the
// same freeze seen again — the other direction, or the next shed frame — and
// only stretches it.
//
// Fed from the capture thread and the input thread, read by the stats tick.
class LinkFreezeLog
{
public:
    static constexpr int64_t kMergeMs = 1000;

    struct Snapshot
    {
        int count = 0;
        int64_t maxMs = 0;
        int64_t lastMs = 0;
    };

    /// The link carried nothing from @p startMs to @p endMs (one clock for
    /// every caller).
    void note(int64_t startMs, int64_t endMs)
    {
        if (endMs < startMs) return;
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Count > 0 && startMs <= m_End + kMergeMs) {
            if (startMs < m_Start) m_Start = startMs;
            if (endMs > m_End) m_End = endMs;
        } else {
            m_Count++;
            m_Start = startMs;
            m_End = endMs;
        }
        const int64_t span = m_End - m_Start;
        if (span > m_MaxMs) m_MaxMs = span;
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return {m_Count, m_MaxMs, m_Count > 0 ? m_End - m_Start : 0};
    }

private:
    mutable std::mutex m_Mutex;
    int m_Count = 0;
    int64_t m_Start = 0;
    int64_t m_End = 0;
    int64_t m_MaxMs = 0;
};
