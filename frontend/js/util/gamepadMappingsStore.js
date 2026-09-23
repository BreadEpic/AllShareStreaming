/*
 * MoonlightWeb — Browser-based Moonlight streaming client.
 * Copyright (C) 2026 Bruno Martin.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * The user's own controller mappings, kept in this browser only.
 *
 * Keyed by pad (gamepadMapping.padKey), never by session or share link: a pad
 * mapped once stays mapped for every later stream, owner or guest. Its own
 * key, not a field of `mw-streaming-settings`, because those settings are
 * pushed to the server as defaults for other browsers — a mapping belongs to
 * the pad in this hand, not to the machine.
 *
 * Every change is announced with a `mw-gamepad-mappings-changed` window event
 * so a running stream picks it up without being told.
 */

const STORAGE_KEY = 'mw-gamepad-mappings';
export const CHANGED_EVENT = 'mw-gamepad-mappings-changed';

function readAll() {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        const parsed = raw ? JSON.parse(raw) : null;
        return parsed && typeof parsed === 'object' ? parsed : {};
    } catch {
        return {};
    }
}

function writeAll(all, key) {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(all));
    } catch {
        /* storage full or blocked: the mapping lasts for this page only */
    }
    try {
        window.dispatchEvent(new CustomEvent(CHANGED_EVENT, { detail: { key } }));
    } catch {
        /* no window (tests) */
    }
}

// What the page saved, also kept here so a blocked localStorage still
// remembers the mapping until the page closes.
let memory = null;

function all() {
    if (!memory) memory = readAll();
    return memory;
}

/** @returns {{name: string, bindings: object, updatedAt: number}|null} */
export function getMapping(key) {
    const m = all()[key];
    return m && m.bindings && typeof m.bindings === 'object' ? m : null;
}

export function setMapping(key, name, bindings) {
    const a = all();
    a[key] = { name: String(name || ''), bindings, updatedAt: Date.now() };
    writeAll(a, key);
}

export function removeMapping(key) {
    const a = all();
    if (!(key in a)) return;
    delete a[key];
    writeAll(a, key);
}

/** Every saved mapping: [{key, name, bindings, updatedAt}], newest first. */
export function listMappings() {
    return Object.entries(all())
        .filter(([, m]) => m && m.bindings)
        .map(([key, m]) => ({ key, ...m }))
        .sort((x, y) => (y.updatedAt || 0) - (x.updatedAt || 0));
}

/** Test hook: forget the in-memory copy so the next read goes to storage. */
export function _resetForTests() {
    memory = null;
}
