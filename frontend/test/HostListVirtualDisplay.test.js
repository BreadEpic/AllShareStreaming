/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
/*
 * The native host card with nothing plugged in.
 *
 * A PC installed through a TV and then left without a screen used to lose its
 * card altogether: the engine answered "no display", ComputerManager dropped
 * the host, and the owner was left with a list that no longer mentioned the
 * machine serving it. Now the card stays, says why it is empty, and offers the
 * fix on the spot. What is verified here is the card's side of that contract:
 *
 *   - the empty state is painted from the server's `nativeDisplay` verdict,
 *     never from the app list (which is not even asked for);
 *   - the kebab offers Add or Remove according to what the server says is
 *     installed, and nothing at all on any other kind of host;
 *   - a display appearing or leaving changes the card's fingerprint, so the
 *     poll repaints it.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAppList: vi.fn(async () => ({ status: 'ok', apps: [] })),
        getVirtualDisplay: vi.fn(),
        removeVirtualDisplay: vi.fn(),
        getVirtualDisplayStatus: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';

const NATIVE = {
    uuid: 'moonlightweb-native-host',
    name: 'UM790 — MoonlightWeb Host',
    state: 'online',
    pairState: 'paired',
    backendType: 'native',
};

const headless = (vd = {}) => ({
    ...NATIVE,
    nativeDisplay: {
        state: 'no_display',
        virtual_display: {
            supported: true,
            installed: false,
            active: false,
            can_install: true,
            ...vd,
        },
    },
});

const withDisplay = (vd = {}) => ({
    ...NATIVE,
    nativeDisplay: {
        state: 'ok',
        virtual_display: {
            supported: true,
            installed: true,
            active: true,
            can_install: true,
            ...vd,
        },
    },
});

describe('the native host card without a display', () => {
    let view;
    let container;

    beforeEach(() => {
        localStorage.clear();
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

    it('stays on the page, empty, with the offer in it — and asks for no apps', () => {
        const card = mount(headless());
        expect(card).not.toBeNull();
        expect(card.querySelector('.host-empty-display')).not.toBeNull();
        expect(card.querySelector('.host-empty-display .btn-vdisplay-add')).not.toBeNull();
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(card.querySelector('.status-badge').textContent).toBe('hosts.statusNoDisplay');
        expect(BackendClient.getAppList).not.toHaveBeenCalled();
    });

    it('offers Add in the menu while nothing is installed, Remove once something is', () => {
        let card = mount(headless());
        expect(card.querySelector('.host-menu .btn-vdisplay-add')).not.toBeNull();
        expect(card.querySelector('.host-menu .btn-vdisplay-remove')).toBeNull();

        view.destroy();
        card = mount(withDisplay());
        expect(card.querySelector('.host-menu .btn-vdisplay-remove')).not.toBeNull();
        expect(card.querySelector('.host-menu .btn-vdisplay-add')).toBeNull();
        // A display is there: the card is an ordinary host again, grid and all.
        expect(card.querySelector('.host-empty-display')).toBeNull();
        expect(card.querySelector('.host-apps')).not.toBeNull();
    });

    it('still offers Add when this install cannot elevate — the dialog explains', () => {
        // The button is not the install; it opens the dialog, which shows the
        // manual path in that case. Hiding it would leave the empty card mute.
        const card = mount(headless({ can_install: false }));
        expect(card.querySelector('.btn-vdisplay-add')).not.toBeNull();
    });

    it('offers neither on a host that is not the native one', () => {
        const card = mount({
            uuid: 'sunshine-1',
            name: 'BENCH',
            state: 'online',
            pairState: 'paired',
        });
        expect(card.querySelector('.btn-vdisplay-add')).toBeNull();
        expect(card.querySelector('.btn-vdisplay-remove')).toBeNull();
        expect(card.querySelector('.host-empty-display')).toBeNull();
    });

    it('offers nothing when the server says the platform cannot do it', () => {
        const card = mount({
            ...NATIVE,
            nativeDisplay: {
                state: 'ok',
                virtual_display: { supported: false, installed: false, active: false },
            },
        });
        expect(card.querySelector('.btn-vdisplay-add')).toBeNull();
        expect(card.querySelector('.btn-vdisplay-remove')).toBeNull();
    });

    it('changes fingerprint when the display state moves, so the poll repaints', () => {
        view = new HostListView(container);
        const before = view._cardFingerprint(new Host(headless()));
        const after = view._cardFingerprint(new Host(withDisplay()));
        expect(before).not.toBe(after);
    });
});

describe('the Host model', () => {
    it('reads needsVirtualDisplay from the server verdict only', () => {
        expect(new Host(headless()).needsVirtualDisplay).toBe(true);
        expect(new Host(withDisplay()).needsVirtualDisplay).toBe(false);
        // A Sunshine host carrying the same object by accident is still not native.
        expect(new Host({ ...headless(), backendType: '' }).needsVirtualDisplay).toBe(false);
        expect(new Host(NATIVE).needsVirtualDisplay).toBe(false);
    });
});
