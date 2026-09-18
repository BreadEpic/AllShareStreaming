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
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

/**
 * @brief Turning "MoonlightWeb Virtual Display" on and off, from a stream's
 * start to the display being there — and back.
 *
 * One operation at a time, fully asynchronous on the Qt main thread: write the
 * request → reach the elevated helper (see VirtualDisplay.h for the three
 * ways) → wait for its result → re-probe the engine → persist. Callers pass a
 * callback; several streams asking for the same thing at once share one
 * operation, and a request that arrives while another runs waits its turn.
 *
 * The off switch is deferred (releaseSoon): a page reload or a quality
 * switch ends one worker and starts another within a second, and toggling a
 * display — moving every window twice — for that would be worse than
 * leaving it on a moment longer.
 */
class VirtualDisplayJob : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(bool ok, const QString& error)>;

    static VirtualDisplayJob& instance();

    enum class State
    {
        Idle,
        Elevating,
        Applying,
        Configuring,
        Refreshing,
        Done,
        Failed
    };

    /// Turn the display on (enable, mode, primary). Answers at once when it
    /// is already on. Cancels a pending releaseSoon().
    void activate(Callback cb);

    /// Turn it off (previous primary back, disable). @p cb may be null.
    void deactivate(Callback cb);

    /// Turn it off in a few seconds unless activate() comes first.
    void releaseSoon();

    bool running() const;

    /// {state, action, error?, started_at, finished_at?, display?}
    QJsonObject statusJson() const;

private:
    explicit VirtualDisplayJob(QObject* parent = nullptr);

    struct Pending
    {
        VirtualDisplay::Request::Action action;
        Callback cb;
    };

    void enqueue(VirtualDisplay::Request::Action action, Callback cb);
    void startNext();
    void setState(State s);
    void fail(const QString& error);
    void succeed(const VirtualDisplay::Result& res);
    void settle(bool ok, const QString& error);

    void dispatch();
    void applyInProcess();
    void makeMain();
    void runHelper(const QStringList& args, bool inConsoleSession);
    void runTask();
    void pollResult();
    void handleResult(const VirtualDisplay::Result& res);

    State m_State = State::Idle;
    VirtualDisplay::Request m_Request;
    QList<Callback> m_Callbacks; ///< who asked for the running operation
    QList<Pending> m_Queue;      ///< what comes after it
    QString m_Error;
    QString m_Display;
    QDateTime m_StartedAt;
    QDateTime m_FinishedAt;

    QTimer m_Poll;
    QTimer m_Deadline;
    QTimer m_Release;
    QByteArray m_HelperOut;
    // The service path runs the elevated half as SYSTEM and the desktop half
    // in the console session; this remembers which half is still to run.
    std::optional<QString> m_NextStage;
    // macOS: the request was applied in this process and the poll is waiting
    // for the OS to list the display; this is the result to deliver then.
    std::optional<VirtualDisplay::Result> m_InProcessResult;
};
