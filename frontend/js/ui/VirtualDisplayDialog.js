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

/**
 * MoonlightWeb — "Add Virtual Display" dialog.
 *
 * A native host with nothing plugged in has nothing to stream. This dialog
 * asks for the one thing the machine cannot guess — the display it should
 * pretend to have (size, refresh rate, HDR, and which GPU when there are
 * several) — and then follows the server's job until the display exists.
 *
 * Everything that decides whether the button may be shown, and whether the
 * machine can do it at all, comes from the server (`can_install`,
 * `os_hdr_capable`, the GPU list). This file never derives any of it: the
 * driver lands on the server's machine, and only the server knows what that
 * machine is.
 *
 * On an install without the elevated task (an older installer, a `--dev`
 * instance) there is no button at all — a link to the driver and the manual
 * steps instead, the same shape as the gamepad notice in that situation.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

const POLL_MS = 1000;

export class VirtualDisplayDialog {
    /**
     * @param {import('../models/Host.js').Host} host the native host card
     * @param {{onDone?: () => void}} [opts] called once the display is there
     */
    constructor(host, { onDone } = {}) {
        this.host = host;
        this.onDone = onDone;
        this.overlay = null;
        this.info = null;
        this.pollTimer = null;
        this.busy = false;
    }

    esc(s) {
        return escapeHtml(s);
    }

    async show() {
        this.render();
        document.body.appendChild(this.overlay);
        this.bindEvents();
        try {
            this.info = await BackendClient.getVirtualDisplay();
        } catch (err) {
            if (!this.overlay) return;
            this.setStatus(err?.message || t('vdisplay.loadFailed'), 'error');
            return;
        }
        if (!this.overlay) return;
        this.renderForm();
        // A job left running by another tab: follow it rather than offer a
        // second one the server would refuse anyway.
        const job = this.info.job;
        if (job && !['idle', 'done', 'failed'].includes(job.state)) {
            this.setBusy(true);
            this.showProgress(job);
            this.startPolling();
        }
    }

    render() {
        this.overlay = document.createElement('div');
        this.overlay.className = 'pairing-overlay vdisplay-overlay';
        this.overlay.innerHTML = `
            <div class="pairing-dialog vdisplay-dialog" role="dialog" aria-modal="true"
                 aria-labelledby="vdisplay-title">
                <h2 id="vdisplay-title">${this.esc(t('vdisplay.title'))}</h2>
                <p class="vdisplay-intro">${this.esc(t(this.introKey()))}</p>
                <div class="vdisplay-form">
                    <p class="vdisplay-loading">${this.esc(t('vdisplay.loading'))}</p>
                </div>
                <p class="vdisplay-status" hidden></p>
                <div class="dialog-actions">
                    <button type="button" class="btn btn-secondary" id="vdisplay-cancel">
                        ${this.esc(t('common.cancel'))}
                    </button>
                    <button type="button" class="btn" id="vdisplay-add" hidden>
                        ${this.esc(t('vdisplay.add'))}
                    </button>
                </div>
            </div>`;
    }

    /** The form, once the server has said what this machine offers. */
    renderForm() {
        const form = this.overlay.querySelector('.vdisplay-form');
        const addBtn = this.overlay.querySelector('#vdisplay-add');
        const info = this.info || {};

        if (!info.supported) {
            form.innerHTML = `<p class="setup-note">${this.esc(t('vdisplay.unsupported'))}</p>`;
            return;
        }
        if (!info.can_install) {
            const url = info.download_url || '';
            form.innerHTML = `
                <p class="setup-note setup-warn">${this.esc(t('vdisplay.manualHint'))}</p>
                <ol class="vdisplay-steps">
                    <li>${this.esc(t('vdisplay.manualStep1'))}</li>
                    <li>${this.esc(t('vdisplay.manualStep2'))}</li>
                    <li>${this.esc(t('vdisplay.manualStep3'))}</li>
                </ol>
                <a class="btn btn-neutral" href="${encodeURI(url)}" target="_blank" rel="noopener">
                    ${this.esc(t('vdisplay.download'))}
                </a>`;
            return;
        }

        const presets = info.presets || {};
        const resolutions = presets.resolutions || [
            [1280, 720],
            [1920, 1080],
            [2560, 1440],
            [3840, 2160],
        ];
        const refresh = presets.refresh || [60, 90, 120];
        const gpus = info.gpus || [];

        const resOptions = resolutions
            .map(([w, h]) => {
                const label = `${w} × ${h}`;
                const sel = w === 1920 && h === 1080 ? ' selected' : '';
                return `<option value="${w}x${h}"${sel}>${label}</option>`;
            })
            .join('');
        const hzOptions = refresh
            .map((hz) => `<option value="${hz}"${hz === 60 ? ' selected' : ''}>${hz} Hz</option>`)
            .join('');
        const gpuOptions = gpus
            .map((g) => `<option value="${this.esc(g.name)}">${this.esc(g.name)}</option>`)
            .join('');

        form.innerHTML = `
            <label class="vdisplay-row">
                <span>${this.esc(t('vdisplay.resolution'))}</span>
                <select id="vdisplay-res">${resOptions}</select>
            </label>
            <label class="vdisplay-row">
                <span>${this.esc(t('vdisplay.refresh'))}</span>
                <select id="vdisplay-hz">${hzOptions}</select>
            </label>
            ${
                info.os_hdr_capable
                    ? `<label class="vdisplay-row vdisplay-check">
                           <input type="checkbox" id="vdisplay-hdr">
                           <span>${this.esc(t('vdisplay.hdr'))}</span>
                       </label>`
                    : ''
            }
            ${
                gpus.length > 1
                    ? `<label class="vdisplay-row">
                           <span>${this.esc(t('vdisplay.gpu'))}</span>
                           <select id="vdisplay-gpu">${gpuOptions}</select>
                       </label>`
                    : ''
            }
            <p class="vdisplay-hint">${this.esc(t(this.hintKey(info)))}</p>`;
        addBtn.hidden = false;
    }

    /**
     * Why the dialog is here: a machine with no screen at all (the headless
     * case), or one that has screens and gets another, at a resolution of the
     * admin's choosing. The card's verdict decides; nothing is derived.
     */
    introKey() {
        return this.host?.needsVirtualDisplay === false ? 'vdisplay.introExtra' : 'vdisplay.intro';
    }

    /**
     * What clicking Add will do on this host, as the server said: download and
     * install a driver (Windows), or create the display right away without
     * installing anything (macOS, `method: 'inprocess'`).
     */
    hintKey(info) {
        return info.method === 'inprocess' ? 'vdisplay.hintInProcess' : 'vdisplay.hint';
    }

    bindEvents() {
        this.overlay
            .querySelector('#vdisplay-cancel')
            .addEventListener('click', () => this.close());
        this.overlay.querySelector('#vdisplay-add').addEventListener('click', () => this.add());
        this.overlay.addEventListener('click', (e) => {
            // The backdrop closes it, but not while a driver is being installed:
            // the job goes on server-side and the page would lose track of it.
            if (e.target === this.overlay && !this.busy) this.close();
        });
    }

    /** What the form says, as the server expects it. */
    readForm() {
        const res = this.overlay.querySelector('#vdisplay-res')?.value || '1920x1080';
        const [w, h] = res.split('x').map((n) => parseInt(n, 10));
        const refresh = parseInt(this.overlay.querySelector('#vdisplay-hz')?.value || '60', 10);
        const hdr = !!this.overlay.querySelector('#vdisplay-hdr')?.checked;
        const gpu = this.overlay.querySelector('#vdisplay-gpu')?.value || '';
        return { width: w, height: h, refresh, hdr, gpu };
    }

    async add() {
        if (this.busy) return;
        this.setBusy(true);
        this.setStatus(t('vdisplay.starting'), 'info');
        try {
            const job = await BackendClient.addVirtualDisplay(this.readForm());
            this.showProgress(job);
            this.startPolling();
        } catch (err) {
            this.setBusy(false);
            this.setStatus(err?.message || t('vdisplay.failed'), 'error');
        }
    }

    startPolling() {
        this.stopPolling();
        this.pollTimer = setInterval(() => this.poll(), POLL_MS);
    }

    stopPolling() {
        if (this.pollTimer) clearInterval(this.pollTimer);
        this.pollTimer = null;
    }

    async poll() {
        if (!this.overlay) return this.stopPolling();
        let job;
        try {
            job = await BackendClient.getVirtualDisplayStatus();
        } catch {
            return; // one missed poll is nothing; the next one will answer
        }
        if (!this.overlay) return this.stopPolling();
        this.showProgress(job);
        if (job.state === 'done') {
            this.stopPolling();
            Toast.show(
                job.reboot_required ? t('vdisplay.doneReboot') : t('vdisplay.done'),
                'success',
            );
            if (this.onDone) this.onDone();
            this.close();
        } else if (job.state === 'failed') {
            this.stopPolling();
            this.setBusy(false);
            this.setStatus(job.error || t('vdisplay.failed'), 'error');
        }
    }

    /** The progress line for a job state. */
    showProgress(job) {
        const key = {
            downloading: 'vdisplay.progressDownloading',
            verifying: 'vdisplay.progressVerifying',
            staging: 'vdisplay.progressVerifying',
            elevating: 'vdisplay.progressInstalling',
            installing: 'vdisplay.progressInstalling',
            configuring: 'vdisplay.progressConfiguring',
            refreshing: 'vdisplay.progressConfiguring',
        }[job?.state];
        if (key) this.setStatus(t(key), 'info');
    }

    setBusy(busy) {
        this.busy = busy;
        const addBtn = this.overlay?.querySelector('#vdisplay-add');
        const cancel = this.overlay?.querySelector('#vdisplay-cancel');
        if (addBtn) addBtn.disabled = busy;
        if (cancel) cancel.disabled = busy;
        this.overlay
            ?.querySelectorAll('.vdisplay-form select, .vdisplay-form input')
            .forEach((el) => (el.disabled = busy));
    }

    setStatus(text, kind) {
        const el = this.overlay?.querySelector('.vdisplay-status');
        if (!el) return;
        el.textContent = text;
        el.className = `vdisplay-status vdisplay-status-${kind}`;
        el.hidden = !text;
    }

    close() {
        this.stopPolling();
        if (this.overlay) this.overlay.remove();
        this.overlay = null;
    }
}
