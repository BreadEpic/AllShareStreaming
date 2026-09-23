/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';
import {
    getMapping,
    setMapping,
    removeMapping,
    listMappings,
    CHANGED_EVENT,
    _resetForTests,
} from '../js/util/gamepadMappingsStore.js';

describe('gamepadMappingsStore', () => {
    beforeEach(() => {
        localStorage.clear();
        _resetForTests();
    });

    it('keeps a mapping in this browser, under its own key', () => {
        setMapping('usb:1234:5678', 'Pad', { a: { t: 'b', i: 2 } });
        expect(getMapping('usb:1234:5678')).toMatchObject({
            name: 'Pad',
            bindings: { a: { t: 'b', i: 2 } },
        });
        // Survives a reload: read back from storage, not from memory.
        _resetForTests();
        expect(getMapping('usb:1234:5678').bindings.a).toEqual({ t: 'b', i: 2 });
        expect(JSON.parse(localStorage.getItem('mw-gamepad-mappings'))).toHaveProperty(
            'usb:1234:5678',
        );
        // Never inside the settings pushed to the server as defaults.
        expect(localStorage.getItem('mw-streaming-settings')).toBeNull();
    });

    it('announces every change', () => {
        const seen = vi.fn();
        window.addEventListener(CHANGED_EVENT, seen);
        setMapping('name:pad', 'Pad', {});
        removeMapping('name:pad');
        removeMapping('name:pad'); // nothing to remove: no event
        window.removeEventListener(CHANGED_EVENT, seen);
        expect(seen).toHaveBeenCalledTimes(2);
    });

    it('lists mappings newest first and forgets removed ones', () => {
        const now = vi.spyOn(Date, 'now');
        now.mockReturnValue(1000);
        setMapping('a', 'A', {});
        now.mockReturnValue(2000);
        setMapping('b', 'B', {});
        now.mockRestore();
        expect(listMappings().map((m) => m.key)).toEqual(['b', 'a']);
        removeMapping('b');
        expect(getMapping('b')).toBeNull();
        expect(listMappings().map((m) => m.key)).toEqual(['a']);
    });

    it('treats unreadable storage as empty', () => {
        localStorage.setItem('mw-gamepad-mappings', '{not json');
        _resetForTests();
        expect(getMapping('a')).toBeNull();
        expect(listMappings()).toEqual([]);
    });
});
