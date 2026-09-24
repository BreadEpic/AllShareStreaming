/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHosts: vi.fn(), getAppList: vi.fn(), wakeHost: vi.fn() },
}));

import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const ASLEEP = {
    uuid: 'host-a',
    name: 'BENCH-DESK',
    state: 'offline',
    pairState: 'paired',
    wakeSupported: true,
};

/**
 * After "Wake" the card has to say something until the machine is up: the
 * packet is fire-and-forget, a PC takes tens of seconds to come out of sleep,
 * and a button that simply comes back looks like nothing happened.
 */
describe('waking a sleeping host', () => {
    let view;
    let container;

    beforeEach(() => {
        vi.clearAllMocks();
        vi.useFakeTimers({ toFake: ['Date'] });
        BackendClient.getAppList.mockReturnValue(new Promise(() => {}));
        BackendClient.wakeHost.mockResolvedValue({ status: 'ok' });
        vi.spyOn(Toast, 'show').mockImplementation(() => {});
        document.body.innerHTML = '<div id="root"></div>';
        container = document.getElementById('root');
        view = new HostListView(container);
        view._active = true;
        view.hosts = [new Host(ASLEEP)];
        view.renderList();
    });

    afterEach(() => {
        if (view) view.destroy();
        view = null;
        localStorage.clear();
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    const wolButton = () => container.querySelector('.btn-wol');
    const toasts = () => Toast.show.mock.calls.map(([message, type]) => [message, type]);
    const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

    const wake = async () => {
        wolButton().click();
        await flush();
    };

    it('sends the packet and shows the card as waking', async () => {
        await wake();
        expect(BackendClient.wakeHost).toHaveBeenCalledWith('host-a');
        expect(wolButton().classList.contains('btn-wol--waking')).toBe(true);
        expect(wolButton().disabled).toBe(true);
        expect(wolButton().textContent).toContain('hosts.wakingBtn');
    });

    it('does not send again while the host is being woken', async () => {
        await wake();
        wolButton().click();
        await flush();
        expect(BackendClient.wakeHost).toHaveBeenCalledTimes(1);
    });

    it('says so once the host is up', async () => {
        await wake();
        BackendClient.getHosts.mockResolvedValue({
            hosts: [{ ...ASLEEP, state: 'online' }],
        });
        await view.refresh();
        expect(toasts()).toContainEqual(['hosts.wakeDone', 'success']);
        expect(container.querySelector('.btn-wol--waking')).toBe(null);
    });

    it('counts a machine answering again as awake, before its server runs', async () => {
        await wake();
        BackendClient.getHosts.mockResolvedValue({ hosts: [{ ...ASLEEP, reachable: true }] });
        await view.refresh();
        expect(toasts()).toContainEqual(['hosts.wakeDone', 'success']);
    });

    it('sends the packet once more after a while', async () => {
        await wake();
        BackendClient.getHosts.mockResolvedValue({ hosts: [ASLEEP] });
        vi.setSystemTime(Date.now() + HostListView.WAKE_RESEND_MS + 1);
        await view.refresh();
        await view.refresh();
        expect(BackendClient.wakeHost).toHaveBeenCalledTimes(2);
        expect(wolButton().classList.contains('btn-wol--waking')).toBe(true);
    });

    it('gives up with a hint when the host never comes up', async () => {
        await wake();
        BackendClient.getHosts.mockResolvedValue({ hosts: [ASLEEP] });
        vi.setSystemTime(Date.now() + HostListView.WAKE_TIMEOUT_MS + 1);
        await view.refresh();
        expect(toasts()).toContainEqual(['hosts.wakeTimeout', 'warning']);
        // The ordinary button is back, ready for another try.
        expect(wolButton().classList.contains('btn-wol--waking')).toBe(false);
        expect(wolButton().disabled).toBe(false);
    });
});
