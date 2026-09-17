/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
/*
 * The "Add Virtual Display" dialog takes every verdict from the server: which
 * options exist (HDR only where the OS can, a GPU choice only with several),
 * whether this install can do it at all (else the manual path), and how the
 * job is going. It sends exactly the chosen preset and follows the job to its
 * end. None of that may be guessed on the page.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getVirtualDisplay: vi.fn(),
        addVirtualDisplay: vi.fn(),
        getVirtualDisplayStatus: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));
vi.mock('../js/ui/Toast.js', () => ({ Toast: { show: vi.fn() } }));

import { VirtualDisplayDialog } from '../js/ui/VirtualDisplayDialog.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const info = (overrides = {}) => ({
    supported: true,
    installed: false,
    active: false,
    can_install: true,
    method: 'task',
    download_url: 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/x.zip',
    os_hdr_capable: true,
    active_displays: [],
    gpus: [{ id: 0, name: 'AMD Radeon 780M' }],
    presets: {
        resolutions: [
            [1280, 720],
            [1920, 1080],
            [2560, 1440],
            [3840, 2160],
        ],
        refresh: [60, 90, 120],
    },
    job: { state: 'idle' },
    ...overrides,
});

const flush = async () => {
    for (let i = 0; i < 6; i++) await Promise.resolve();
};

describe('VirtualDisplayDialog', () => {
    let dialog;

    beforeEach(() => {
        vi.clearAllMocks();
        vi.useFakeTimers();
        document.body.innerHTML = '';
    });

    afterEach(() => {
        if (dialog) dialog.close();
        dialog = null;
        vi.useRealTimers();
    });

    const open = async (i) => {
        BackendClient.getVirtualDisplay.mockResolvedValue(i);
        dialog = new VirtualDisplayDialog({ uuid: 'moonlightweb-native-host' });
        await dialog.show();
        await flush();
        return document.body;
    };

    it('shows HDR only where the OS can, and a GPU choice only with several', async () => {
        let body = await open(info());
        expect(body.querySelector('#vdisplay-hdr')).not.toBeNull();
        expect(body.querySelector('#vdisplay-gpu')).toBeNull();
        dialog.close();

        body = await open(
            info({
                os_hdr_capable: false,
                gpus: [
                    { id: 0, name: 'RTX 5060 Ti' },
                    { id: 1, name: 'Arc A380' },
                ],
            }),
        );
        expect(body.querySelector('#vdisplay-hdr')).toBeNull();
        expect(body.querySelector('#vdisplay-gpu')).not.toBeNull();
        expect(body.querySelectorAll('#vdisplay-gpu option')).toHaveLength(2);
    });

    it('sends the chosen preset and follows the job to done', async () => {
        const body = await open(info());
        body.querySelector('#vdisplay-res').value = '2560x1440';
        body.querySelector('#vdisplay-hz').value = '120';
        body.querySelector('#vdisplay-hdr').checked = true;

        BackendClient.addVirtualDisplay.mockResolvedValue({ state: 'downloading', action: 'add' });
        BackendClient.getVirtualDisplayStatus
            .mockResolvedValueOnce({ state: 'installing' })
            .mockResolvedValueOnce({ state: 'done', reboot_required: false });
        const onDone = vi.fn();
        dialog.onDone = onDone;

        body.querySelector('#vdisplay-add').click();
        await flush();
        expect(BackendClient.addVirtualDisplay).toHaveBeenCalledWith({
            width: 2560,
            height: 1440,
            refresh: 120,
            hdr: true,
            gpu: '',
        });
        expect(body.querySelector('.vdisplay-status').textContent).toBe(
            'vdisplay.progressDownloading',
        );

        await vi.advanceTimersByTimeAsync(1000);
        await flush();
        expect(body.querySelector('.vdisplay-status').textContent).toBe(
            'vdisplay.progressInstalling',
        );

        await vi.advanceTimersByTimeAsync(1000);
        await flush();
        expect(onDone).toHaveBeenCalled();
        expect(Toast.show).toHaveBeenCalledWith('vdisplay.done', 'success');
        expect(document.querySelector('.vdisplay-overlay')).toBeNull();
    });

    it('keeps the dialog with the error when the job fails', async () => {
        const body = await open(info());
        BackendClient.addVirtualDisplay.mockResolvedValue({ state: 'downloading' });
        BackendClient.getVirtualDisplayStatus.mockResolvedValue({
            state: 'failed',
            error: 'The downloaded driver package does not match the published one',
        });
        body.querySelector('#vdisplay-add').click();
        await flush();
        await vi.advanceTimersByTimeAsync(1000);
        await flush();

        expect(document.querySelector('.vdisplay-overlay')).not.toBeNull();
        expect(body.querySelector('.vdisplay-status').textContent).toContain('does not match');
        // Usable again: the person can retry or give up.
        expect(body.querySelector('#vdisplay-add').disabled).toBe(false);
    });

    it('shows the manual path, not a button, when this install cannot elevate', async () => {
        const body = await open(info({ can_install: false }));
        expect(body.querySelector('#vdisplay-add').hidden).toBe(true);
        expect(body.querySelector('.vdisplay-steps')).not.toBeNull();
        const link = body.querySelector('.vdisplay-form a');
        expect(link.getAttribute('href')).toContain('github.com/VirtualDrivers');
    });

    it('follows a job another tab started rather than offering a second one', async () => {
        BackendClient.getVirtualDisplayStatus.mockResolvedValue({ state: 'installing' });
        const body = await open(info({ job: { state: 'installing', action: 'add' } }));
        expect(body.querySelector('#vdisplay-add').disabled).toBe(true);
        expect(body.querySelector('.vdisplay-status').textContent).toBe(
            'vdisplay.progressInstalling',
        );
        expect(BackendClient.addVirtualDisplay).not.toHaveBeenCalled();
    });
});
