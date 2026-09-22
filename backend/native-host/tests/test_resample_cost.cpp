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

#include "convert/ResampleCost.h"
#include "native_test_framework.h"

using mw::native::convert::ResampleCost;

void run_resample_cost_tests()
{
    SECTION("ResampleCost — the warm-up frames are never timed");
    {
        ResampleCost cost;
        for (int i = 0; i < ResampleCost::kWarmupFrames; ++i)
            CHECK(!cost.timeThisFrame(0));
        CHECK(cost.timeThisFrame(0));
    }

    SECTION("ResampleCost — no more timings in flight than samples still wanted");
    {
        ResampleCost cost;
        for (int i = 0; i < ResampleCost::kWarmupFrames; ++i)
            cost.timeThisFrame(0);
        CHECK(cost.timeThisFrame(ResampleCost::kSamples - 1));
        CHECK(!cost.timeThisFrame(ResampleCost::kSamples));
    }

    SECTION("ResampleCost — the median decides, not a stray frame");
    {
        ResampleCost cost;
        CHECK_EQ(cost.medianUs(), int64_t(-1));
        // The Arc's 0.8 ms, with a few frames that shared the GPU with a burst
        // of desktop work.
        for (size_t i = 0; i < ResampleCost::kSamples; ++i)
            cost.add(i % 10 == 0 ? 9000 : 800);
        CHECK(cost.done());
        CHECK_EQ(cost.medianUs(), int64_t(800));
        CHECK(ResampleCost::affordable(cost.medianUs()));
        // Nothing more is taken, nor timed, once the answer is in.
        cost.add(50000);
        CHECK_EQ(cost.medianUs(), int64_t(800));
        CHECK(!cost.timeThisFrame(0));
    }

    SECTION("ResampleCost — the smallest AMD iGPU is over budget, the Arc under");
    {
        CHECK(!ResampleCost::affordable(4500));
        CHECK(ResampleCost::affordable(800));
        CHECK(ResampleCost::affordable(ResampleCost::kBudgetUs));
        CHECK(!ResampleCost::affordable(ResampleCost::kBudgetUs + 1));
        CHECK(!ResampleCost::affordable(-1));
    }

    SECTION("ResampleCost — reset starts over, warm-up included");
    {
        ResampleCost cost;
        for (size_t i = 0; i < ResampleCost::kSamples; ++i)
            cost.add(4500);
        CHECK(cost.done());
        cost.reset();
        CHECK(!cost.done());
        CHECK(!cost.timeThisFrame(0));
    }
}
