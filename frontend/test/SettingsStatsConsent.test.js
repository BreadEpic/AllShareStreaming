/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What this machine reports, and the switch that stops it, on the Settings
 * page. It lives there rather than in Admin because it concerns everyone who
 * streams through this machine — so the disclosure has to reach a plain user,
 * while the switch itself stays the machine's own.
 *
 * Not a consent: the census carries no field that identifies anyone, so no
 * question is put on arrival and what is owed is exactly these two halves.
 * Both are privacy properties, and each one failing is silent at runtime,
 * leaving a checkbox that shows a state the backend does not hold, or a user
 * who was never told what is counted.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        getMetricsReporting: vi.fn(),
        setMetricsReporting: vi.fn(async () => ({ enabled: true })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('SettingsView privacy section', () => {
    let view;

    const load = async (state) => {
        BackendClient.getMetricsReporting.mockResolvedValue(state);
        await view._loadStatsConsent();
        return view._renderPrivacySection();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
    });

    it('starts with no section at all, rather than a wrong one', () => {
        expect(view._statsLoaded).toBe(false);
        expect(view._renderPrivacySection()).toBe('');
    });

    it('shows the switch, ticked, to the machine that may answer', async () => {
        const html = await load({ enabled: true, available: true, writable: true });
        expect(html).toContain('settings-stats-consent');
        expect(html).toContain('checked');
        expect(html).not.toContain('disabled');
        expect(html).not.toContain('text:stats.ownerOnly');
    });

    it('shows the switch unticked once this machine has opted out', async () => {
        const html = await load({ enabled: false, available: true, writable: true });
        expect(html).toContain('settings-stats-consent');
        expect(html).not.toContain('checked');
    });

    // The whole reason it moved out of Admin: a signed-in user on another
    // machine gets to read what is counted about their streams. What they do
    // not get is the answer — that speaks for the machine, not for them.
    //
    // And it is stated, never shown as a control: a ticked box nobody can
    // untick reads as a setting forced on the reader, which is the one thing
    // this section must not do.
    it('tells a remote user what the machine does, with no switch at all', async () => {
        const html = await load({ enabled: true, available: true, writable: false });
        expect(html).not.toContain('settings-stats-consent');
        expect(html).not.toContain('checkbox');
        expect(html).toContain('text:stats.remoteOn');
        expect(html).toContain('text:stats.ownerOnly');
        expect(html).toContain('text:stats.statsSent1');
        expect(html).toContain('text:stats.statsNever3');
    });

    it('tells a remote user nothing is sent when that machine has opted out', async () => {
        const html = await load({ enabled: false, available: true, writable: false });
        expect(html).toContain('text:stats.remoteOff');
        expect(html).not.toContain('text:stats.remoteOn');
    });

    it('says so plainly on a build that reports nothing, instead of a switch that lies', async () => {
        const html = await load({ enabled: false, available: false, writable: true });
        expect(html).toContain('text:stats.unavailable');
        expect(html).not.toContain('settings-stats-consent');
    });

    it('always carries the full disclosure, whichever way the switch is set', async () => {
        const html = await load({ enabled: false, available: true, writable: true });
        for (const key of ['sentIntro', 'statsSent1', 'neverIntro', 'statsNever3', 'necessary']) {
            expect(html).toContain('text:stats.' + key);
        }
    });

    it('hides the section when the backend will not say — never blocks the page', async () => {
        BackendClient.getMetricsReporting.mockRejectedValue(new Error('offline'));
        await view._loadStatsConsent();
        expect(view._statsLoaded).toBe(false);
        expect(view._renderPrivacySection()).toBe('');
    });

    // The means of refusal the disclosure promises: unticking has to reach the
    // backend, and it has to be the only thing the page needs to do.
    it('turns the census off from the switch', async () => {
        const box = { checked: false, disabled: false };
        await view._setStatsConsent(box);
        expect(BackendClient.setMetricsReporting).toHaveBeenCalledWith(false);
        expect(view._statsGranted).toBe(false);
        expect(box.disabled).toBe(false);
    });

    it('turns it back on the same way', async () => {
        const box = { checked: true, disabled: false };
        await view._setStatsConsent(box);
        expect(BackendClient.setMetricsReporting).toHaveBeenCalledWith(true);
        expect(view._statsGranted).toBe(true);
    });

    it('puts the switch back when the choice could not be saved', async () => {
        BackendClient.setMetricsReporting.mockRejectedValue(new Error('offline'));
        const box = { checked: false, disabled: false };
        await view._setStatsConsent(box);
        // The backend is still reporting, so the box must not claim otherwise.
        expect(box.checked).toBe(true);
        expect(box.disabled).toBe(false);
    });
});
