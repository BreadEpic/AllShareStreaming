/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/ControlChannel.h"

#include <QHostAddress>
#include <QTcpServer>

// Two editions on one PC (an installed DEV beside a --dev instance) derive the
// same control port. The second one used to keep the taken number, and the
// HttpServer proxied its tabs' /ws/control to the OTHER instance (23/09/2026).
void run_control_channel_tests()
{
    SECTION("ControlChannel");

    QTcpServer squatter;
    CHECK(squatter.listen(QHostAddress::LocalHost, 0));
    const quint16 taken = squatter.serverPort();

    ControlChannel channel(taken);
    CHECK(channel.start());
    CHECK(channel.port() != 0);
    CHECK(channel.port() != taken);

    // A free port is still taken as asked.
    const quint16 wanted = channel.port();
    channel.stop();
    ControlChannel again(wanted);
    CHECK(again.start());
    CHECK_EQ(again.port(), wanted);
}
