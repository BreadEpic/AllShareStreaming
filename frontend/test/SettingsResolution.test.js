/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice on the Settings page, and the ratio it retired. The
 * ratio dropdown left the product in 0.3.1: a user must not find it, a value
 * they forced before must not survive a save, and picking any resolution
 * puts the ratio back to Auto — while a debug build keeps all of it for tests.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getMetricsConsent: vi.fn(),
        setMetricsConsent: vi.fn(async () => ({ decision: 'granted' })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => 'text:' + key + (params ? ':' + JSON.stringify(params) : ''),
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';

const STORAGE_KEY = 'mw-streaming-settings';

describe('SettingsView resolution', () => {
    let view;

    const stored = () => JSON.parse(localStorage.getItem(STORAGE_KEY));
    const select = () => view.container.querySelector('#settings-stream-height');
    const pick = async (value) => {
        select().value = value;
        select().dispatchEvent(new Event('change'));
        // _autoSave debounces 300 ms.
        await new Promise((r) => setTimeout(r, 350));
    };

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
    });

    it('offers the three choices after the fixed rungs', () => {
        view.render();
        const values = [...select().options].map((o) => o.value);
        expect(values).toEqual(['720', '1080', '1440', '2160', 'device', 'host', 'custom']);
        expect(select().value).toBe('1080');
        expect(view.container.querySelector('#settings-custom-width')).toBeNull();
    });

    it('hides the ratio from a user, and shows it to a debug build', () => {
        view.render();
        expect(view.container.querySelector('#settings-stream-aspect')).toBeNull();
        view._debugBuild = true;
        view.render();
        expect(view.container.querySelector('#settings-stream-aspect')).not.toBeNull();
    });

    // The 0.3.1 migration: a "4:3" forced under 0.3.0 is Auto from the next
    // save on, and never marked as a deliberate override.
    it('saves the ratio as Auto whatever was stored, outside a debug build', async () => {
        view._applySettings({ stream_aspect: '4:3', stream_height: 1440 });
        expect(view._streamAspect).toBe('4:3');
        view.render();
        view.bindEvents();
        expect(view._streamAspect).toBe('auto');
        await view._saveToStorage();
        expect(stored().stream_aspect).toBe('auto');
        expect(stored().stream_aspect_forced).toBeUndefined();
        expect(stored().stream_height).toBe(1440);
        expect(stored().stream_resolution).toBe('fixed');
    });

    it('keeps a forced ratio in a debug build, and says so', async () => {
        view._debugBuild = true;
        view._applySettings({ stream_aspect: '4:3' });
        view.render();
        view.bindEvents();
        await view._saveToStorage();
        expect(stored().stream_aspect).toBe('4:3');
        expect(stored().stream_aspect_forced).toBe(true);
    });

    it('puts the ratio back to Auto when a resolution is picked', async () => {
        view._debugBuild = true;
        view._applySettings({ stream_aspect: '4:3' });
        view.render();
        view.bindEvents();
        await pick('device');
        expect(view._streamAspect).toBe('auto');
        expect(stored().stream_aspect).toBe('auto');
        expect(stored().stream_resolution).toBe('device');
        // The rung is kept for the day the user comes back to it.
        expect(stored().stream_height).toBe(1080);
    });

    it('shows the custom pair with its choice, pinned into bounds and even', async () => {
        view.render();
        view.bindEvents();
        await pick('custom');
        const w = view.container.querySelector('#settings-custom-width');
        const h = view.container.querySelector('#settings-custom-height');
        expect(w).not.toBeNull();
        expect(w.value).toBe('1920');
        expect(h.value).toBe('1080');
        w.value = '9999';
        w.dispatchEvent(new Event('change'));
        h.value = '767';
        h.dispatchEvent(new Event('change'));
        await new Promise((r) => setTimeout(r, 350));
        expect(w.value).toBe('4096');
        expect(h.value).toBe('766');
        expect(stored().stream_custom_width).toBe(4096);
        expect(stored().stream_custom_height).toBe(766);
        expect(stored().stream_resolution).toBe('custom');
        // Back to a rung: the pair goes, the value it held stays stored.
        await pick('720');
        expect(view.container.querySelector('#settings-custom-width')).toBeNull();
        expect(stored().stream_resolution).toBe('fixed');
        expect(stored().stream_height).toBe(720);
        expect(stored().stream_custom_width).toBe(4096);
    });

    it('reads an unknown stored choice as the fixed rung', () => {
        view._applySettings({ stream_resolution: 'native', stream_height: 2160 });
        view.render();
        expect(select().value).toBe('2160');
    });
});
