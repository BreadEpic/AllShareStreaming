/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
/*
 * A Mac host whose privacy grants went away after the install (an update
 * signed differently, a tccutil reset). Screen Recording gone: the card used
 * to vanish from the list; now it stays and says which switch to flip, and
 * asks for no apps. Accessibility gone: it still streams, so the grid stays,
 * with the warning above it — macOS itself says nothing.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAppList: vi.fn(async () => ({ status: 'ok', apps: [] })),
        getVirtualDisplay: vi.fn(),
        openScreenRecordingSettings: vi.fn(async () => ({})),
        openAccessibilitySettings: vi.fn(async () => {
            throw new Error('403');
        }),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const MAC = {
    uuid: 'moonlightweb-native-host',
    name: 'Mac — MoonlightWeb Host',
    state: 'online',
    pairState: 'paired',
    backendType: 'native',
};
const mac = (nativeDisplay) => ({ ...MAC, nativeDisplay });

describe('a Mac host that lost a privacy grant', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
    });
    afterEach(() => {
        if (view) view.destroy();
        view = null;
    });

    const mount = (data) => {
        view = new HostListView(container);
        view.hosts = [new Host(data)];
        view.renderList();
        return container.querySelector('.host-card');
    };

    it('Screen Recording gone: the card stays, says so, and asks for no apps', () => {
        const card = mount(mac({ state: 'no_capture_permission', input_permission: true }));
        expect(card).not.toBeNull();
        expect(card.querySelector('.host-empty-display-text').textContent).toBe(
            'hosts.nativeCapturePermission',
        );
        expect(card.querySelector('.btn-mac-perm').dataset.pane).toBe('screen');
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });

    it('Accessibility gone: the grid stays, with the warning above it', () => {
        const card = mount(mac({ state: 'ok', input_permission: false }));
        expect(card.querySelector('.host-perm-warn p').textContent).toBe(
            'hosts.nativeInputPermission',
        );
        expect(card.querySelector('.btn-mac-perm').dataset.pane).toBe('accessibility');
        expect(card.querySelector('.host-apps')).not.toBeNull();
    });

    it('both grants present: an ordinary card', () => {
        const card = mount(mac({ state: 'ok', input_permission: true }));
        expect(card.querySelector('.host-perm-warn')).toBeNull();
        expect(card.querySelector('.btn-mac-perm')).toBeNull();
    });

    it('the button opens the pane on the Mac, or says to go there', async () => {
        const card = mount(mac({ state: 'ok', input_permission: false }));
        card.querySelector('.btn-mac-perm').click();
        expect(BackendClient.openAccessibilitySettings).toHaveBeenCalled();
    });

    it('the model reads both verdicts, and only for the native host', () => {
        expect(new Host(mac({ state: 'no_capture_permission' })).needsCapturePermission).toBe(true);
        expect(new Host(mac({ state: 'ok', input_permission: false })).lacksInputPermission).toBe(
            true,
        );
        expect(new Host(mac({ state: 'ok' })).lacksInputPermission).toBe(false);
        expect(
            new Host({ ...mac({ state: 'ok', input_permission: false }), backendType: '' })
                .lacksInputPermission,
        ).toBe(false);
    });
});
