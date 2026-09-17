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

#include <QString>

/**
 * @brief The `--vdisplay-apply` mode: the part of adding a virtual display
 * that needs rights the server does not have.
 *
 * Started by the elevated scheduled task (stage "all"), by the SYSTEM service
 * as a child (stage "driver") followed by a console-session child (stage
 * "mode"), or by a hand-elevated instance. It reads request.json from the
 * staging directory, does exactly what that says, writes result.json and
 * prints the same JSON line on stdout. Never a window, never a message box:
 * the task runs hidden on a desktop that may have no monitor at all.
 *
 * Stage "driver" (elevated):
 *   add    — re-hash the three driver files against the constants compiled in
 *            here, write the driver's settings XML, trust the catalog's
 *            signer for the duration, create the Root\MttVDD node and install
 *            the package on it (SetupAPI + newdev), untrust again.
 *   remove — remove the device node, delete the package from the driver
 *            store, delete the settings file.
 * Stage "mode" (any desktop process): wait for the virtual display to appear,
 *   set the requested resolution and refresh rate, switch HDR on or off.
 */
namespace VirtualDisplayApply {

/// @param stage "all" | "driver" | "mode"
/// @param dir   staging directory; empty = VirtualDisplay::stagingDir()
/// @return process exit code: 0 ok, 1 failed, 3010 ok but reboot required
int run(const QString& stage, const QString& dir);

} // namespace VirtualDisplayApply
