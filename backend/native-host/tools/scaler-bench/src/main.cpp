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

// mw-scaler-bench — which downscale filter should the native host use?
//
// For every GPU of the machine, every (source → output) case a picture is
// available for, SDR and HDR, and every candidate filter: time the pass alone
// with GPU timestamps, score its output against a CPU reference, save crops,
// and write an interactive report. See tools/scaler-bench/CMakeLists.txt for
// the why, and report.html for how to read the result.

#include "Adapters.h"
#include "Bench.h"
#include "Image.h"
#include "Metrics.h"
#include "Pass.h"
#include "Reference.h"
#include "Report.h"
#include "Scalers.h"
#include "Timing.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace bench {

const char* toString(Range r)
{
    return r == Range::Hdr ? "hdr" : "sdr";
}
const char* toString(Space s)
{
    switch (s) {
    case Space::Perceptual: return "perceptual";
    case Space::Linear: return "linear";
    default: return "driver";
    }
}
const char* toString(Precision p)
{
    switch (p) {
    case Precision::Fp32: return "fp32";
    case Precision::Fp16: return "fp16";
    default: return "driver";
    }
}
const char* toString(Kind k)
{
    switch (k) {
    case Kind::Pixel: return "pixel";
    case Kind::Mip: return "mip";
    case Kind::Compute: return "compute";
    default: return "videoprocessor";
    }
}

} // namespace bench

using namespace bench;

namespace {

struct Options
{
    std::vector<std::wstring> sources;
    std::wstring outDir;
    std::wstring shaderDir;
    std::wstring templatePath;
    std::set<int> adapters; // empty = all
    std::set<std::string> cases;
    std::set<std::string> variants;
    int iterations = 300;
    int warmup = 30;
    int batch = 50;
    double trim = 0.10;
    bool hdr = true;
    bool crops = true;
    int cropGpu = 0;
    int cropW = 160;
    int cropH = 100;
};

void usage()
{
    std::puts(
        "mw-scaler-bench [options]\n"
        "  --source <png>       a source picture (repeatable; picked per case by its size).\n"
        "                       Default: C:\\Test\\00_Background.png and C:\\Test\\01_4K.png\n"
        "  --out <dir>          output directory (default C:\\Test\\scaler-bench\\<date>)\n"
        "  --adapters all|i,j   GPU indices to run on (default all)\n"
        "  --cases a,b          case ids (4k-1440p 4k-1080p 4k-720p 1440p-1080p 1440p-720p)\n"
        "  --variants a,b       variant ids or families to restrict to\n"
        "  --iterations <n>     timed runs per candidate (default 300)\n"
        "  --warmup <n>         untimed runs first (default 30)\n"
        "  --batch <n>          runs per disjoint query (default 50)\n"
        "  --trim <f>           share cut off each end for the trimmed mean (default 0.10)\n"
        "  --no-hdr             SDR only\n"
        "  --no-crops           no crop images\n"
        "  --crop-gpu <i>       GPU whose outputs are cropped (default 0)\n"
        "  --shaders <dir>      shader directory (default <exe>\\scaler-bench\\shaders)\n"
        "  --template <file>    report template (default <exe>\\scaler-bench\\report.html)\n");
}

std::wstring widen(const std::string& s)
{
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--source" && next(v)) {
            o.sources.push_back(v);
        } else if (a == L"--out" && next(v)) {
            o.outDir = v;
        } else if (a == L"--shaders" && next(v)) {
            o.shaderDir = v;
        } else if (a == L"--template" && next(v)) {
            o.templatePath = v;
        } else if (a == L"--adapters" && next(v)) {
            if (v != L"all")
                for (const std::string& s : split(narrow(v)))
                    o.adapters.insert(std::stoi(s));
        } else if (a == L"--cases" && next(v)) {
            for (const std::string& s : split(narrow(v)))
                o.cases.insert(s);
        } else if (a == L"--variants" && next(v)) {
            for (const std::string& s : split(narrow(v)))
                o.variants.insert(s);
        } else if (a == L"--iterations" && next(v)) {
            o.iterations = std::stoi(v);
        } else if (a == L"--warmup" && next(v)) {
            o.warmup = std::stoi(v);
        } else if (a == L"--batch" && next(v)) {
            o.batch = std::max(1, std::stoi(v));
        } else if (a == L"--trim" && next(v)) {
            o.trim = std::stod(v);
        } else if (a == L"--crop-gpu" && next(v)) {
            o.cropGpu = std::stoi(v);
        } else if (a == L"--no-hdr") {
            o.hdr = false;
        } else if (a == L"--no-crops") {
            o.crops = false;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            usage();
            return false;
        } else {
            std::wprintf(L"unknown argument: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

std::string now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm = {};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::wstring stamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm = {};
    localtime_s(&tm, &t);
    wchar_t buf[32];
    std::wcsftime(buf, 32, L"%Y%m%d-%H%M", &tm);
    return buf;
}

/// The scroll test moves the source by this many pixels, both axes: one, so
/// that every sub-pixel phase of the output changes — a whole ratio period
/// would leave any filter's output identical and measure nothing.
constexpr int kScrollShift = 1;

std::vector<Case> allCases()
{
    struct Def
    {
        const char* id;
        const char* label;
        Size src;
        Size dst;
    };
    const Def defs[] = {
        {"4k-1440p", "4K → 1440p", {3840, 2160}, {2560, 1440}},
        {"4k-1080p", "4K → 1080p", {3840, 2160}, {1920, 1080}},
        {"4k-720p", "4K → 720p", {3840, 2160}, {1280, 720}},
        {"1440p-1080p", "1440p → 1080p", {2560, 1440}, {1920, 1080}},
        {"1440p-720p", "1440p → 720p", {2560, 1440}, {1280, 720}},
    };
    std::vector<Case> out;
    for (const Def& d : defs) {
        Case c;
        c.id = d.id;
        c.label = d.label;
        c.src = d.src;
        c.dst = d.dst;
        out.push_back(c);
    }
    return out;
}

/// A loaded source picture, by size.
struct Picture
{
    std::wstring path;
    ImageRgba8 rgba;
};

std::string cropName(const std::string& caseId, Range range, const std::string& who, int i)
{
    return "crops/" + caseId + "-" + toString(range) + "-" + who + "-" + std::to_string(i) + ".png";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) return 1;

    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::puts("CoInitialize failed");
        return 1;
    }

    // Defaults relative to the executable.
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path exeDir = fs::path(exePath).parent_path();
    if (o.shaderDir.empty()) o.shaderDir = (exeDir / L"scaler-bench" / L"shaders").wstring();
    if (o.templatePath.empty())
        o.templatePath = (exeDir / L"scaler-bench" / L"report.html").wstring();
    if (o.outDir.empty()) o.outDir = L"C:\\Test\\scaler-bench\\" + stamp();
    if (o.sources.empty()) {
        o.sources = {L"C:\\Test\\00_Background.png", L"C:\\Test\\01_4K.png"};
    }

    SessionData session;
    session.date = now();
    for (int i = 0; i < argc; ++i)
        session.commandLine += (i ? " " : "") + narrow(argv[i]);
    session.shaderDir = narrow(o.shaderDir);
    session.iterations = o.iterations;
    session.warmup = o.warmup;
    session.batch = o.batch;
    session.trim = o.trim;

    std::error_code ec;
    fs::create_directories(fs::path(o.outDir) / L"crops", ec);
    if (ec) {
        std::wprintf(L"cannot create %s\n", o.outDir.c_str());
        return 1;
    }

    // ── Pictures ──
    std::vector<Picture> pictures;
    for (const std::wstring& path : o.sources) {
        Picture p;
        p.path = path;
        std::string err;
        if (!fs::exists(path)) {
            session.messages.push_back("source not found: " + narrow(path));
            continue;
        }
        if (!image::loadPng(path, p.rgba, err)) {
            session.messages.push_back("source " + narrow(path) + ": " + err);
            continue;
        }
        std::wprintf(L"source %s: %dx%d\n", path.c_str(), p.rgba.w, p.rgba.h);
        pictures.push_back(std::move(p));
    }

    // ── Cases ──
    std::vector<Case> cases;
    for (Case c : allCases()) {
        if (!o.cases.empty() && !o.cases.count(c.id)) continue;
        for (const Picture& p : pictures) {
            if (p.rgba.w == c.src.w && p.rgba.h == c.src.h) {
                c.sourcePath = narrow(p.path);
                break;
            }
        }
        if (c.sourcePath.empty()) {
            session.messages.push_back("case " + c.id + " skipped: no " + std::to_string(c.src.w) +
                                       "x" + std::to_string(c.src.h) +
                                       " picture was given (never fabricated by upscaling)");
        }
        cases.push_back(c);
    }
    session.cases = cases;

    // ── GPUs ──
    std::string err;
    std::vector<Gpu> gpus = enumerateGpus(session.skippedGpus, err);
    if (gpus.empty()) {
        std::printf("no GPU: %s\n", err.c_str());
        return 1;
    }
    for (const Gpu& g : gpus) {
        session.gpus.push_back(g.info);
        std::printf("gpu %d: %s (driver %s, FL %s, fp16 %s%s)\n", g.info.index, g.info.name.c_str(),
                    g.info.driver.c_str(), g.info.featureLevel.c_str(),
                    g.info.fp16 ? "honoured" : "ignored", g.info.displayGpu ? ", display" : "");
        if (!g.info.fp16) {
            session.messages.push_back(g.info.name +
                                       ": the driver reports no 16-bit min-precision support; "
                                       "its fp16 candidates ran at fp32.");
        }
    }

    // ── Variants ──
    std::vector<Variant> variants;
    for (const Variant& v : allVariants()) {
        if (!o.variants.empty() && !o.variants.count(v.id) && !o.variants.count(v.family)) continue;
        variants.push_back(v);
    }
    session.variants = variants;

    const std::vector<Range> ranges =
        o.hdr ? std::vector<Range>{Range::Sdr, Range::Hdr} : std::vector<Range>{Range::Sdr};

    // ── The run ──
    for (const Case& c : cases) {
        if (c.sourcePath.empty()) continue;
        const Picture* picture = nullptr;
        for (const Picture& p : pictures)
            if (narrow(p.path) == c.sourcePath) picture = &p;
        if (!picture) continue;

        for (Range range : ranges) {
            std::printf("\n== %s, %s ==\n", c.label.c_str(), toString(range));

            // CPU side: the input in its encoded and linear forms, the
            // shifted twin, the reference, the crop regions.
            SourceInfo info;
            info.caseId = c.id;
            info.range = range;
            ImageF encoded;
            ImageF linear;
            if (range == Range::Sdr) {
                encoded = ImageF(picture->rgba.w, picture->rgba.h);
                const size_t n = static_cast<size_t>(encoded.w) * encoded.h;
                for (size_t i = 0; i < n; ++i)
                    for (int ch = 0; ch < 3; ++ch)
                        encoded.px[i * 3 + ch] = picture->rgba.px[i * 4 + ch] / 255.0f;
                linear = image::decodeSrgb(picture->rgba);
                info.description = "8-bit sRGB, as loaded.";
            } else {
                linear = image::synthesizeHdr(image::decodeSrgb(picture->rgba), info.description);
                encoded = image::pqEncode(linear);
            }
            const ImageF encodedShifted = image::shifted(encoded, kScrollShift, kScrollShift);
            const ImageF linearShifted = image::shifted(linear, kScrollShift, kScrollShift);

            std::printf("reference (Lanczos-3, linear light, %dx%d), twice...\n", c.dst.w, c.dst.h);
            const ImageF refLinear = reference::downscale(linear, c.dst.w, c.dst.h);
            const ImageF ref =
                range == Range::Sdr ? image::srgbEncode(refLinear) : image::pqEncode(refLinear);
            const ImageF refShiftedLinear = reference::downscale(linearShifted, c.dst.w, c.dst.h);
            const ImageF refShifted = range == Range::Sdr ? image::srgbEncode(refShiftedLinear)
                                                          : image::pqEncode(refShiftedLinear);
            const float* luma = metrics::lumaWeights(range);
            info.referenceHf = metrics::hfRatio(ref, luma);
            info.regions = metrics::pickCropRegions(ref, o.cropW, o.cropH, luma);
            if (o.crops) {
                for (size_t i = 0; i < info.regions.size(); ++i) {
                    const CropRegion& r = info.regions[i];
                    std::string e;
                    const std::string refName =
                        cropName(c.id, range, "reference", static_cast<int>(i));
                    image::savePng(fs::path(o.outDir) / widen(refName),
                                   image::crop8(ref, r.x, r.y, r.w, r.h), e);
                    info.referenceCrops.push_back(refName);
                    // The same window of the source, at the source's scale.
                    const double sx = static_cast<double>(c.src.w) / c.dst.w;
                    const double sy = static_cast<double>(c.src.h) / c.dst.h;
                    const std::string srcName =
                        cropName(c.id, range, "source", static_cast<int>(i));
                    image::savePng(fs::path(o.outDir) / widen(srcName),
                                   image::crop8(encoded, static_cast<int>(r.x * sx),
                                                static_cast<int>(r.y * sy),
                                                static_cast<int>(r.w * sx),
                                                static_cast<int>(r.h * sy)),
                                   e);
                    info.sourceCrops.push_back(srcName);
                }
            }
            session.sources.push_back(info);

            for (Gpu& gpu : gpus) {
                if (!o.adapters.empty() && !o.adapters.count(gpu.info.index)) continue;
                std::printf("-- %s\n", gpu.info.name.c_str());

                SourceSet sources;
                sources.init(gpu.device.Get(), range, &encoded, &encodedShifted,
                             range == Range::Hdr ? &linear : nullptr,
                             range == Range::Hdr ? &linearShifted : nullptr);

                // Build every candidate first, so the timing can go round
                // the lot and spread clocks and heat evenly.
                struct Live
                {
                    const Variant* variant;
                    std::unique_ptr<Pass> pass;
                    std::vector<double> us;
                    int discarded = 0;
                };
                std::vector<Live> live;
                for (const Variant& v : variants) {
                    Result r;
                    r.gpu = gpu.info.index;
                    r.caseId = c.id;
                    r.range = range;
                    r.variant = v.id;
                    auto pass = std::make_unique<Pass>();
                    std::string reason;
                    if (!pass->init(gpu, v, c, range, sources, o.shaderDir, reason)) {
                        r.status = pass->unsupported() ? "unsupported" : "failed";
                        r.reason = reason;
                        std::printf("   %-32s %s: %s\n", v.id.c_str(), r.status.c_str(),
                                    reason.c_str());
                        session.results.push_back(r);
                        continue;
                    }
                    live.push_back({&v, std::move(pass), {}, 0});
                }

                // Warm-up: the shader caches and first-use costs for each,
                // then a third of a second of back-to-back work so the clocks
                // are up before the first timestamp (the Arc drops them the
                // moment it idles between batches, 17/09/2026).
                GpuTimer timer;
                std::string terr;
                if (!timer.init(gpu.device.Get(), gpu.context.Get(), o.batch, terr)) {
                    std::printf("   timer: %s\n", terr.c_str());
                    continue;
                }
                for (Live& l : live) {
                    for (int i = 0; i < o.warmup; ++i)
                        l.pass->run(false);
                    timer.drain();
                }
                if (!live.empty()) {
                    // One round at a time, drained: enough to keep the
                    // clocks up, never an unbounded queue (which is what
                    // made an RTX refuse a query at 4K, 17/09/2026).
                    const auto until =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < until) {
                        for (Live& l : live)
                            l.pass->run(false);
                        timer.drain();
                    }
                }
                const int rounds = (o.iterations + o.batch - 1) / o.batch;
                for (int round = 0; round < rounds; ++round) {
                    for (Live& l : live) {
                        if (l.variant->kind == Kind::VideoProcessor) {
                            timer.measureBatchWall([&] { l.pass->run(false); }, l.us);
                            continue;
                        }
                        int attempts = 0;
                        while (!timer.measureBatch([&] { l.pass->run(false); }, l.us) &&
                               ++attempts < 5) {
                            l.discarded++;
                        }
                    }
                }

                // Quality: one run of each, read back and scored; then the
                // shifted input for the phase figure; then the crops.
                for (Live& l : live) {
                    Result r;
                    r.gpu = gpu.info.index;
                    r.caseId = c.id;
                    r.range = range;
                    r.variant = l.variant->id;
                    r.status = "ok";
                    r.notes = l.pass->notes();
                    r.hasTime = true;
                    r.time = computeStats(l.us, o.trim, l.discarded);
                    if (l.variant->kind == Kind::VideoProcessor) r.time.method = "wall-clock";

                    ImageF out;
                    ImageF outShifted;
                    std::string rerr;
                    l.pass->run(false);
                    if (!l.pass->readback(out, rerr)) {
                        r.status = "failed";
                        r.reason = rerr;
                        session.results.push_back(r);
                        continue;
                    }
                    l.pass->run(true);
                    if (!l.pass->readback(outShifted, rerr)) {
                        r.status = "failed";
                        r.reason = rerr;
                        session.results.push_back(r);
                        continue;
                    }
                    r.hasQuality = true;
                    r.quality.psnrY = metrics::psnr(out, ref, true, luma, 0, 0, c.dst.w, c.dst.h);
                    r.quality.psnrRgb =
                        metrics::psnr(out, ref, false, luma, 0, 0, c.dst.w, c.dst.h);
                    r.quality.ssimY = metrics::ssimLuma(out, ref, luma);
                    metrics::temporal(out, outShifted, ref, refShifted, luma, r.quality.flicker,
                                      r.quality.motionGain, r.quality.temporalPsnr);
                    r.quality.hfRatio = metrics::hfRatio(out, luma);

                    if (o.crops && l.variant->crops && gpu.info.index == o.cropGpu) {
                        for (size_t i = 0; i < info.regions.size(); ++i) {
                            const CropRegion& reg = info.regions[i];
                            const std::string name =
                                cropName(c.id, range, l.variant->id, static_cast<int>(i));
                            std::string e;
                            if (image::savePng(fs::path(o.outDir) / widen(name),
                                               image::crop8(out, reg.x, reg.y, reg.w, reg.h), e))
                                r.crops.push_back(name);
                        }
                    }
                    std::printf("   %-32s %8.1f us%s (med %7.1f, p95 %7.1f)  PSNR %5.2f  SSIM "
                                "%.4f  flicker %.4f  gain %.3f  tPSNR %5.2f  HF %.4f\n",
                                l.variant->id.c_str(), r.time.trimmedUs,
                                r.time.method == "wall-clock" ? "w" : " ", r.time.medianUs,
                                r.time.p95Us, r.quality.psnrY, r.quality.ssimY, r.quality.flicker,
                                r.quality.motionGain, r.quality.temporalPsnr, r.quality.hfRatio);
                    session.results.push_back(r);
                }

                // An fp16 candidate that does not agree with its fp32 twin is
                // a broken half path, not a faster one (FSR1 on NVIDIA's fxc
                // min16float, 17/09/2026). Say so next to its figures.
                for (Result& r : session.results) {
                    if (r.gpu != gpu.info.index || r.caseId != c.id || r.range != range ||
                        !r.hasQuality)
                        continue;
                    const std::string suffix = "-fp16";
                    if (r.variant.size() <= suffix.size() ||
                        r.variant.compare(r.variant.size() - suffix.size(), suffix.size(),
                                          suffix) != 0)
                        continue;
                    const std::string twin = r.variant.substr(0, r.variant.size() - suffix.size());
                    for (const Result& t : session.results) {
                        if (t.gpu != r.gpu || t.caseId != r.caseId || t.range != r.range ||
                            t.variant != twin || !t.hasQuality)
                            continue;
                        const double delta = std::abs(t.quality.psnrY - r.quality.psnrY);
                        if (delta > 1.0) {
                            std::ostringstream n;
                            n << (r.notes.empty() ? "" : r.notes + "; ")
                              << "fp16 output differs from the fp32 twin by " << std::fixed
                              << std::setprecision(1) << delta
                              << " dB: this driver's half-precision path is not trustworthy for "
                                 "this shader";
                            r.notes = n.str();
                        }
                    }
                }
            }
        }
    }

    std::string werr;
    if (!writeReport(o.outDir, o.templatePath, session, werr)) {
        std::printf("report: %s\n", werr.c_str());
        return 1;
    }
    std::wprintf(L"\nreport: %s\\report.html\n", o.outDir.c_str());
    ::CoUninitialize();
    return 0;
}
