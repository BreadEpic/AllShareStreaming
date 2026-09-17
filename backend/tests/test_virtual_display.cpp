/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Covers the rules behind "Add Virtual Display": what an elevated helper will
 * act on, and who may ask for it. The threat this encodes is the request file:
 * it is read by a process running with the user's admin token, so its contents
 * must be a set of choices — never free-form input that ends up in a file the
 * driver reads or in a command line. And the button is offered to the ADMIN,
 * wherever they sit, not merely to whoever is signed in.
 */
#include "test_framework.h"
#include "backend/VirtualDisplay.h"

#include <QRegularExpression>

namespace {

VirtualDisplay::Status supported()
{
    VirtualDisplay::Status st;
    st.supported = true;
    return st;
}

VirtualDisplay::Status unsupported()
{
    return VirtualDisplay::Status{};
}

QByteArray addJson(int w, int h, int hz, const char* extra = "")
{
    return QByteArray("{\"action\":\"add\",\"width\":") + QByteArray::number(w) +
           ",\"height\":" + QByteArray::number(h) + ",\"refresh\":" + QByteArray::number(hz) +
           extra + "}";
}

bool isHex(const QString& s, int len)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]+$"));
    return s.size() == len && hex.match(s).hasMatch();
}

} // namespace

void run_virtual_display_tests()
{
    using namespace VirtualDisplay;

    SECTION("VirtualDisplay — the admin privilege is the gate, wherever the admin sits");
    CHECK(mayManage(true, supported()));
    CHECK(!mayManage(false, supported()));
    // A platform without the feature has nothing to offer even to the admin.
    CHECK(!mayManage(true, unsupported()));

    SECTION("VirtualDisplay — the task is named per edition");
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), false),
             QStringLiteral("MoonlightWeb Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWebDev"), false),
             QStringLiteral("MoonlightWebDev Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), true),
             QStringLiteral("MoonlightWeb-dev Virtual Display"));
    CHECK_EQ(applyArgument(), QStringLiteral("--vdisplay-apply"));

    SECTION("VirtualDisplay — only the presets get through the request parser");
    QString err;
    {
        const auto req =
            parseRequest(addJson(2560, 1440, 120, ",\"hdr\":true,\"gpu\":\"RTX\""), &err);
        CHECK(req.has_value());
        if (req) {
            CHECK(req->action == Request::Action::Add);
            CHECK_EQ(req->width, 2560);
            CHECK_EQ(req->height, 1440);
            CHECK_EQ(req->refresh, 120);
            CHECK(req->hdr);
            CHECK_EQ(req->gpu, QStringLiteral("RTX"));
        }
    }
    // Every preset is accepted; anything else is not a choice we made.
    for (const Preset& p : resolutionPresets())
        for (int hz : refreshPresets())
            CHECK(parseRequest(addJson(p.width, p.height, hz), &err).has_value());
    CHECK(!parseRequest(addJson(1234, 567, 60), &err).has_value());
    CHECK(!parseRequest(addJson(1920, 1080, 61), &err).has_value());
    CHECK(!parseRequest(addJson(0, 0, 0), &err).has_value());
    // Missing fields are not defaults: they are a malformed request.
    CHECK(!parseRequest("{\"action\":\"add\"}", &err).has_value());
    CHECK(!parseRequest("{\"action\":\"format-c\"}", &err).has_value());
    CHECK(!parseRequest("not json", &err).has_value());
    CHECK(!parseRequest("[1,2]", &err).has_value());
    // The GPU name lands in a file an elevated process writes: no control
    // characters, no novel.
    CHECK(!parseRequest(addJson(1920, 1080, 60, ",\"gpu\":\"a\\u0000b\""), &err).has_value());
    CHECK(!parseRequest(addJson(1920, 1080, 60, ",\"gpu\":\"a\\nb\""), &err).has_value());
    CHECK(!parseRequest(addJson(1920, 1080, 60, ",\"gpu\":42"), &err).has_value());
    CHECK(!parseRequest(addJson(1920, 1080, 60, ",\"hdr\":\"yes\""), &err).has_value());
    {
        const QByteArray longName = QByteArray(200, 'x');
        CHECK(!parseRequest(addJson(1920, 1080, 60, ",\"gpu\":\"" + longName + "\""), &err)
                   .has_value());
    }
    // Remove needs no mode at all.
    {
        const auto req = parseRequest("{\"action\":\"remove\"}", &err);
        CHECK(req.has_value());
        if (req) CHECK(req->action == Request::Action::Remove);
    }
    // Round trip.
    {
        Request r;
        r.width = 3840;
        r.height = 2160;
        r.refresh = 90;
        r.hdr = true;
        r.gpu = QStringLiteral("Radeon 780M");
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->width, 3840);
            CHECK_EQ(back->refresh, 90);
            CHECK(back->hdr);
            CHECK_EQ(back->gpu, QStringLiteral("Radeon 780M"));
        }
    }

    SECTION("VirtualDisplay — the result file is read only once complete");
    CHECK(!parseResult("").has_value());
    CHECK(!parseResult("{\"st").has_value());
    CHECK(!parseResult("{}").has_value());
    {
        Result r;
        r.ok = true;
        r.stage = QStringLiteral("done");
        r.rebootRequired = true;
        r.display = QStringLiteral("\\\\.\\DISPLAY3");
        const auto back = parseResult(toJson(r));
        CHECK(back.has_value());
        if (back) {
            CHECK(back->ok);
            CHECK(back->rebootRequired);
            CHECK_EQ(back->display, QStringLiteral("\\\\.\\DISPLAY3"));
        }
        // One line, so a reader taking the last stdout line gets all of it.
        CHECK_EQ(toJson(r).count('\n'), 1);
    }

    SECTION("VirtualDisplay — the driver settings put the choice first and escape the GPU");
    {
        Request r;
        r.width = 2560;
        r.height = 1440;
        r.refresh = 120;
        r.hdr = true;
        r.gpu = QStringLiteral("AMD & <Radeon>");
        const QString xml = settingsXml(r);
        CHECK(xml.contains(QStringLiteral("<count>1</count>")));
        CHECK(
            xml.contains(QStringLiteral("<friendlyname>AMD &amp; &lt;Radeon&gt;</friendlyname>")));
        CHECK(!xml.contains(QStringLiteral("<Radeon>")));
        CHECK(xml.contains(QStringLiteral("<HDRPlus>true</HDRPlus>")));
        // The requested mode is the first <resolution> and the first refresh.
        const int firstRes = xml.indexOf(QStringLiteral("<resolution>"));
        const int chosen = xml.indexOf(QStringLiteral("<width>2560</width>"));
        CHECK(firstRes >= 0 && chosen > firstRes);
        CHECK(xml.indexOf(QStringLiteral("<width>1920</width>")) > chosen);
        CHECK(xml.indexOf(QStringLiteral("<g_refresh_rate>120</g_refresh_rate>")) <
              xml.indexOf(QStringLiteral("<g_refresh_rate>60</g_refresh_rate>")));
    }
    {
        Request r; // defaults: 1080p60, no HDR, default GPU
        const QString xml = settingsXml(r);
        CHECK(xml.contains(QStringLiteral("<friendlyname>default</friendlyname>")));
        CHECK(xml.contains(QStringLiteral("<HDRPlus>false</HDRPlus>")));
    }

    SECTION("VirtualDisplay — the pinned asset is stated in one place, GitHub only");
    CHECK(downloadUrl().startsWith(
        QStringLiteral("https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/")));
    CHECK(isHex(downloadSha256(), 64));
    CHECK(isHex(catalogSignerThumbprint(), 40));
    for (const DriverFile& f : driverFiles()) {
        CHECK(isHex(QString::fromLatin1(f.sha256), 64));
        CHECK(QString::fromLatin1(f.name).startsWith(QStringLiteral("VirtualDisplayDriver/")));
    }
    CHECK_EQ(hardwareId(), QStringLiteral("Root\\MttVDD"));
}
