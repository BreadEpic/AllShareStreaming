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

#pragma once

#include "backend/VirtualDisplay.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;

/**
 * @brief The add/remove operation, from the click to the display being there.
 *
 * One at a time, fully asynchronous on the Qt main thread (event-driven, like
 * GamepadDriver::install): download → verify → extract → verify each file →
 * write the request → reach the elevated helper (see VirtualDisplay.h for the
 * three ways) → wait for its result → re-probe the engine → persist.
 *
 * The browser polls statusJson() every second; the states are the progress
 * text it shows.
 */
class VirtualDisplayJob : public QObject
{
    Q_OBJECT

public:
    static VirtualDisplayJob& instance();

    enum class State
    {
        Idle,
        Downloading,
        Verifying,
        Staging,
        Elevating,
        Installing,
        Configuring,
        Refreshing,
        Done,
        Failed
    };

    /// Start an operation. Returns an English reason when it cannot start:
    /// one already running, platform unsupported, no way to elevate.
    QString start(const VirtualDisplay::Request& req);

    bool running() const;

    /// {state, error?, reboot_required?, started_at, finished_at?, display?}
    QJsonObject statusJson() const;

private:
    explicit VirtualDisplayJob(QObject* parent = nullptr);

    void setState(State s);
    void fail(const QString& error);
    void succeed(const VirtualDisplay::Result& res);

    void download();
    void extract();
    void dispatch();
    void runHelper(const QStringList& args, bool inConsoleSession);
    void runTask();
    void pollResult();
    void handleResult(const VirtualDisplay::Result& res);
    void refreshEngine();

    State m_State = State::Idle;
    VirtualDisplay::Request m_Request;
    QString m_Error;
    bool m_RebootRequired = false;
    QString m_Display;
    QDateTime m_StartedAt;
    QDateTime m_FinishedAt;

    QNetworkAccessManager* m_Nam = nullptr;
    QTimer m_Poll;
    QTimer m_Deadline;
    QByteArray m_HelperOut;
    // The service path runs the driver stage as SYSTEM and the mode stage in
    // the console session; this remembers which half is in flight.
    bool m_ModeStagePending = false;
};
