/*
 * MoonlightWeb — native capture & encoding engine: scaler bench.
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

#include "Bench.h"
#include "Scalers.h"

#include <string>
#include <vector>

namespace bench {

/// Everything one run produced, as the report and results.json see it.
struct SessionData
{
    std::string date;
    std::string commandLine;
    std::string shaderDir;
    int iterations = 0;
    int warmup = 0;
    int batch = 0;
    double trim = 0.1;
    std::vector<GpuInfo> gpus;
    std::vector<std::string> skippedGpus;
    std::vector<Case> cases;
    std::vector<Variant> variants;
    std::vector<SourceInfo> sources;
    std::vector<Result> results;
    /// Anything the reader should know that is not a figure: skipped cases,
    /// missing pictures, drivers that ignore fp16.
    std::vector<std::string> messages;
};

/// The whole session as JSON — the report's data, and results.json.
std::string toJson(const SessionData& session);

/// results.json and report.html (the template with the JSON dropped in) under
/// @p outDir. The template is read from @p templatePath.
bool writeReport(const std::wstring& outDir, const std::wstring& templatePath,
                 const SessionData& session, std::string& error);

} // namespace bench
