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

#include "Report.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace bench {

namespace {

/// A minimal JSON writer: enough for flat records and arrays of them, with
/// the escaping done once and correctly.
class Json
{
public:
    std::string str() const { return m_Out.str(); }

    void beginObject()
    {
        comma();
        m_Out << '{';
        m_First = true;
    }
    void endObject()
    {
        m_Out << '}';
        m_First = false;
    }
    void beginArray(const char* key = nullptr)
    {
        comma();
        if (key) m_Out << '"' << key << "\":";
        m_Out << '[';
        m_First = true;
    }
    void endArray()
    {
        m_Out << ']';
        m_First = false;
    }
    void key(const char* k)
    {
        comma();
        m_Out << '"' << k << "\":";
        m_First = true; // the value follows without a comma
    }
    void value(const std::string& s)
    {
        comma();
        m_Out << '"' << escape(s) << '"';
    }
    void value(const char* s) { value(std::string(s)); }
    void value(double d)
    {
        comma();
        if (!std::isfinite(d)) {
            m_Out << "null";
            return;
        }
        m_Out << std::setprecision(10) << d;
    }
    void value(int i)
    {
        comma();
        m_Out << i;
    }
    void value(uint64_t u)
    {
        comma();
        m_Out << u;
    }
    void value(bool b)
    {
        comma();
        m_Out << (b ? "true" : "false");
    }
    template <typename T> void field(const char* k, const T& v)
    {
        key(k);
        value(v);
    }

private:
    void comma()
    {
        if (!m_First) m_Out << ',';
        m_First = false;
    }
    static std::string escape(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    std::ostringstream m_Out;
    bool m_First = true;
};

std::string narrow(const std::wstring& s)
{
    std::string out;
    for (wchar_t c : s)
        out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

} // namespace

std::string toJson(const SessionData& s)
{
    Json j;
    j.beginObject();
    j.field("tool", "mw-scaler-bench");
    j.field("date", s.date);
    j.field("commandLine", s.commandLine);
    j.field("shaderDir", s.shaderDir);
    j.field("iterations", s.iterations);
    j.field("warmup", s.warmup);
    j.field("batch", s.batch);
    j.field("trim", s.trim);

    j.beginArray("gpus");
    for (const GpuInfo& g : s.gpus) {
        j.beginObject();
        j.field("index", g.index);
        j.field("name", g.name);
        j.field("luid", g.luid);
        j.field("vendorId", static_cast<int>(g.vendorId));
        j.field("deviceId", static_cast<int>(g.deviceId));
        j.field("driver", g.driver);
        j.field("featureLevel", g.featureLevel);
        j.field("fp16", g.fp16);
        j.field("displayGpu", g.displayGpu);
        j.field("vramMb", g.vramMb);
        j.endObject();
    }
    j.endArray();

    j.beginArray("skippedGpus");
    for (const std::string& m : s.skippedGpus)
        j.value(m);
    j.endArray();

    j.beginArray("cases");
    for (const Case& c : s.cases) {
        j.beginObject();
        j.field("id", c.id);
        j.field("label", c.label);
        j.key("src");
        j.beginArray();
        j.value(c.src.w);
        j.value(c.src.h);
        j.endArray();
        j.key("dst");
        j.beginArray();
        j.value(c.dst.w);
        j.value(c.dst.h);
        j.endArray();
        j.field("source", c.sourcePath);
        j.endObject();
    }
    j.endArray();

    j.beginArray("variants");
    for (const Variant& v : s.variants) {
        j.beginObject();
        j.field("id", v.id);
        j.field("family", v.family);
        j.field("label", v.label);
        j.field("note", v.note);
        j.field("kind", toString(v.kind));
        j.field("space", toString(v.space));
        j.field("precision", toString(v.precision));
        j.field("sdr", v.sdr);
        j.field("hdr", v.hdr);
        j.endObject();
    }
    j.endArray();

    j.beginArray("sources");
    for (const SourceInfo& src : s.sources) {
        j.beginObject();
        j.field("case", src.caseId);
        j.field("range", toString(src.range));
        j.field("description", src.description);
        j.field("referenceHf", src.referenceHf);
        j.beginArray("regions");
        for (const CropRegion& r : src.regions) {
            j.beginObject();
            j.field("x", r.x);
            j.field("y", r.y);
            j.field("w", r.w);
            j.field("h", r.h);
            j.field("tag", r.tag);
            j.endObject();
        }
        j.endArray();
        j.beginArray("referenceCrops");
        for (const std::string& p : src.referenceCrops)
            j.value(p);
        j.endArray();
        j.beginArray("sourceCrops");
        for (const std::string& p : src.sourceCrops)
            j.value(p);
        j.endArray();
        j.endObject();
    }
    j.endArray();

    j.beginArray("results");
    for (const Result& r : s.results) {
        j.beginObject();
        j.field("gpu", r.gpu);
        j.field("case", r.caseId);
        j.field("range", toString(r.range));
        j.field("variant", r.variant);
        j.field("status", r.status);
        j.field("reason", r.reason);
        j.field("notes", r.notes);
        if (r.hasTime) {
            j.key("time");
            j.beginObject();
            j.field("trimmed", r.time.trimmedUs);
            j.field("median", r.time.medianUs);
            j.field("p95", r.time.p95Us);
            j.field("min", r.time.minUs);
            j.field("n", r.time.n);
            j.field("discarded", r.time.discarded);
            j.field("method", r.time.method);
            j.endObject();
        }
        if (r.hasQuality) {
            j.key("quality");
            j.beginObject();
            j.field("psnrY", r.quality.psnrY);
            j.field("psnrRgb", r.quality.psnrRgb);
            j.field("ssimY", r.quality.ssimY);
            j.field("flicker", r.quality.flicker);
            j.field("motionGain", r.quality.motionGain);
            j.field("temporalPsnr", r.quality.temporalPsnr);
            j.field("hf", r.quality.hfRatio);
            j.endObject();
        }
        j.beginArray("crops");
        for (const std::string& p : r.crops)
            j.value(p);
        j.endArray();
        j.endObject();
    }
    j.endArray();

    j.beginArray("messages");
    for (const std::string& m : s.messages)
        j.value(m);
    j.endArray();

    j.endObject();
    return j.str();
}

bool writeReport(const std::wstring& outDir, const std::wstring& templatePath,
                 const SessionData& session, std::string& error)
{
    const std::string json = toJson(session);

    {
        std::ofstream f(outDir + L"\\results.json", std::ios::binary);
        if (!f) {
            error = "could not write results.json";
            return false;
        }
        f << json;
    }

    std::ifstream t(templatePath, std::ios::binary);
    if (!t) {
        error = "could not read the report template " + narrow(templatePath);
        return false;
    }
    std::string html((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    const std::string marker = "/*__RESULTS_JSON__*/null";
    const size_t at = html.find(marker);
    if (at == std::string::npos) {
        error = "the report template has no results marker";
        return false;
    }
    // "</script>" inside a string would end the script block early.
    std::string safe = json;
    for (size_t p = safe.find("</"); p != std::string::npos; p = safe.find("</", p + 3))
        safe.replace(p, 2, "<\\/");
    html.replace(at, marker.size(), safe);

    std::ofstream f(outDir + L"\\report.html", std::ios::binary);
    if (!f) {
        error = "could not write report.html";
        return false;
    }
    f << html;
    return true;
}

} // namespace bench
