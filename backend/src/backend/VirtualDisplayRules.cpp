/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "backend/VirtualDisplay.h"

#include "common/Edition.h"

#include <QJsonArray>
#include <QJsonDocument>

// The platform-free half of VirtualDisplay: names, the request/result files,
// the driver's settings XML. Kept apart from VirtualDisplay.cpp so the
// security test target can link it without SetupAPI, the probe service or a
// driver — these are the rules that decide what an elevated process will do,
// and they are verified on every platform (tests/test_virtual_display.cpp).

namespace VirtualDisplay {

QString taskNameFor(const QString& productName, bool devFlag)
{
    return productName + (devFlag ? QStringLiteral("-dev") : QString()) +
           QStringLiteral(" Virtual Display");
}

QString taskName()
{
    return taskNameFor(mw::edition::productName(), mw::edition::devFlag());
}

namespace {

bool isPresetResolution(int w, int h)
{
    for (const Preset& p : resolutionPresets())
        if (p.width == w && p.height == h) return true;
    return false;
}

bool isPresetRefresh(int hz)
{
    for (int r : refreshPresets())
        if (r == hz) return true;
    return false;
}

QString xmlEscape(QString s)
{
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

} // namespace

std::optional<Request> parseRequest(const QByteArray& json, QString* error)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("request is not a JSON object");
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();

    Request req;
    const QString action = obj.value(QLatin1String("action")).toString();
    if (action == QLatin1String("add"))
        req.action = Request::Action::Add;
    else if (action == QLatin1String("remove"))
        req.action = Request::Action::Remove;
    else {
        if (error) *error = QStringLiteral("unknown action");
        return std::nullopt;
    }
    if (req.action == Request::Action::Remove) return req;

    req.width = obj.value(QLatin1String("width")).toInt();
    req.height = obj.value(QLatin1String("height")).toInt();
    req.refresh = obj.value(QLatin1String("refresh")).toInt();
    if (!isPresetResolution(req.width, req.height)) {
        if (error) *error = QStringLiteral("resolution is not one of the presets");
        return std::nullopt;
    }
    if (!isPresetRefresh(req.refresh)) {
        if (error) *error = QStringLiteral("refresh rate is not one of the presets");
        return std::nullopt;
    }
    const QJsonValue hdr = obj.value(QLatin1String("hdr"));
    if (!hdr.isUndefined() && !hdr.isBool()) {
        if (error) *error = QStringLiteral("hdr must be a boolean");
        return std::nullopt;
    }
    req.hdr = hdr.toBool(false);

    const QJsonValue gpu = obj.value(QLatin1String("gpu"));
    if (!gpu.isUndefined() && !gpu.isNull() && !gpu.isString()) {
        if (error) *error = QStringLiteral("gpu must be a string");
        return std::nullopt;
    }
    req.gpu = gpu.toString().trimmed();
    if (req.gpu.size() > 128) {
        if (error) *error = QStringLiteral("gpu name too long");
        return std::nullopt;
    }
    for (const QChar c : req.gpu) {
        // A name goes into an XML file an elevated helper writes: printable
        // only. Control characters have no place in an adapter's name.
        if (c.category() == QChar::Other_Control) {
            if (error) *error = QStringLiteral("gpu name contains control characters");
            return std::nullopt;
        }
    }
    return req;
}

QByteArray toJson(const Request& req)
{
    QJsonObject obj;
    obj["action"] =
        req.action == Request::Action::Add ? QStringLiteral("add") : QStringLiteral("remove");
    if (req.action == Request::Action::Add) {
        obj["width"] = req.width;
        obj["height"] = req.height;
        obj["refresh"] = req.refresh;
        obj["hdr"] = req.hdr;
        obj["gpu"] = req.gpu;
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

std::optional<Result> parseResult(const QByteArray& json)
{
    if (json.trimmed().isEmpty()) return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!obj.contains(QLatin1String("ok"))) return std::nullopt;

    Result res;
    res.ok = obj.value(QLatin1String("ok")).toBool(false);
    res.stage = obj.value(QLatin1String("stage")).toString();
    res.error = obj.value(QLatin1String("error")).toString();
    res.rebootRequired = obj.value(QLatin1String("reboot_required")).toBool(false);
    res.display = obj.value(QLatin1String("display")).toString();
    return res;
}

QByteArray toJson(const Result& res)
{
    QJsonObject obj;
    obj["ok"] = res.ok;
    obj["stage"] = res.stage;
    if (!res.error.isEmpty()) obj["error"] = res.error;
    obj["reboot_required"] = res.rebootRequired;
    if (!res.display.isEmpty()) obj["display"] = res.display;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QString settingsXml(const Request& req)
{
    // The layout the driver ships (its own sample, verified on the bench):
    //   <monitors><count>, <gpu><friendlyname>, <global><g_refresh_rate>*,
    //   <resolutions><resolution>{width,height,refresh_rate}*, <options>.
    // The chosen mode goes first — the driver takes the first as the default —
    // then every preset, so a later mode change needs no driver reload.
    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<vdd_settings>\n");
    xml += QStringLiteral("  <monitors>\n    <count>1</count>\n  </monitors>\n");
    xml += QStringLiteral("  <gpu>\n    <friendlyname>%1</friendlyname>\n  </gpu>\n")
               .arg(req.gpu.isEmpty() ? QStringLiteral("default") : xmlEscape(req.gpu));
    xml += QStringLiteral("  <global>\n");
    xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(req.refresh);
    for (int r : refreshPresets())
        if (r != req.refresh)
            xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(r);
    xml += QStringLiteral("  </global>\n  <resolutions>\n");
    auto one = [&](int w, int h, int hz) {
        xml +=
            QStringLiteral("    <resolution>\n      <width>%1</width>\n      <height>%2</height>\n"
                           "      <refresh_rate>%3</refresh_rate>\n    </resolution>\n")
                .arg(w)
                .arg(h)
                .arg(hz);
    };
    one(req.width, req.height, req.refresh);
    for (const Preset& p : resolutionPresets())
        if (p.width != req.width || p.height != req.height) one(p.width, p.height, req.refresh);
    xml += QStringLiteral("  </resolutions>\n  <options>\n");
    xml += QStringLiteral("    <CustomEdid>false</CustomEdid>\n"
                          "    <PreventSpoof>false</PreventSpoof>\n"
                          "    <EdidCeaOverride>false</EdidCeaOverride>\n"
                          "    <HardwareCursor>true</HardwareCursor>\n");
    xml += QStringLiteral("    <SDR10bit>%1</SDR10bit>\n    <HDRPlus>%1</HDRPlus>\n")
               .arg(req.hdr ? QStringLiteral("true") : QStringLiteral("false"));
    xml += QStringLiteral("    <logging>false</logging>\n    <debuglogging>false</debuglogging>\n"
                          "  </options>\n</vdd_settings>\n");
    return xml;
}

} // namespace VirtualDisplay
