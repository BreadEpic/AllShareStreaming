/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// jsdom has no Pointer Lock: say the mouse can be captured, as on a desktop.
vi.mock('../js/util/BrowserDetect.js', async (importOriginal) => ({
    ...(await importOriginal()),
    CAN_CAPTURE_MOUSE: true,
}));

import { StreamView } from '../js/ui/StreamView.js';

/**
 * Desktop mode sends the mouse as positions, and a position stops at the edge
 * of the viewer's screen: in a game that turns its camera with the mouse
 * (Roblox first person, "Doors") a full turn was impossible — the camera
 * stopped where the viewer's pointer met the side. When the host hides its
 * pointer while ours moves over the picture, the next click or key captures
 * the mouse and motion goes out as deltas; the host showing its pointer again
 * gives it back.
 */
function makeView({ gaming = false } = {}) {
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        _gamingMode: gaming,
        inputEl: document.createElement('div'),
        pointerLocked: false,
        _mouseFocused: false,
        _hostDrawsCursor: false,
        _hostCursorVisible: false,
        _hostCursorHiddenSince: 0,
        _autoCaptureArmed: false,
        _onAutoCaptureKey: null,
        _autoCaptureHintShown: false,
        _lastClientMoveMs: 0,
        _lastMouseClientX: 100,
        _lastMouseClientY: 100,
        _mediaRect: () => ({ left: 0, top: 0, width: 1920, height: 1080 }),
        _showTransientHint: vi.fn(),
        _onPointerLockChange: () => {},
        _lockPointer: vi.fn(() => Promise.resolve()),
    });
    return v;
}

/** The host reports its pointer hidden (a game took the mouse) some time ago. */
function hostHidesPointer(v, msAgo) {
    v._hostDrawsCursor = true;
    v._hostCursorVisible = false;
    v._hostCursorHiddenSince = performance.now() - msAgo;
}

/** Our pointer has just moved over the picture. */
function pointerMoving(v) {
    v._lastClientMoveMs = performance.now();
}

const key = (k) => document.dispatchEvent(new KeyboardEvent('keydown', { key: k }));

describe('capturing the mouse for a game in desktop mode', () => {
    let v;

    afterEach(() => {
        if (v) v._disarmAutoCapture();
        vi.restoreAllMocks();
    });

    beforeEach(() => {
        v = makeView();
    });

    it('arms when the host hides its pointer while ours moves over the picture', () => {
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._onHostCursorVisibility();
        expect(v._autoCaptureArmed).toBe(true);
        expect(v._showTransientHint).toHaveBeenCalledWith('stream.autoCaptureHint', 3000);
    });

    it('captures on the next key', () => {
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._onHostCursorVisibility();
        key('w');
        expect(v._lockPointer).toHaveBeenCalledTimes(1);
        expect(v._autoCaptureArmed).toBe(false);
    });

    it('does not spend the capture on Escape — the key that opens the game menu', () => {
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._onHostCursorVisibility();
        key('Escape');
        expect(v._lockPointer).not.toHaveBeenCalled();
        expect(v._autoCaptureArmed).toBe(true);
    });

    it('leaves a pointer hidden a moment ago alone (a video player showing it again)', () => {
        hostHidesPointer(v, 50);
        pointerMoving(v);
        v._onHostCursorVisibility();
        expect(v._autoCaptureArmed).toBe(false);
    });

    it('leaves a pointer the host hid while ours sat still alone', () => {
        hostHidesPointer(v, 1000);
        v._onHostCursorVisibility();
        expect(v._autoCaptureArmed).toBe(false);
    });

    it('ignores a pointer over the bars beside the picture', () => {
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._lastMouseClientX = 2500;
        v._onHostCursorVisibility();
        expect(v._autoCaptureArmed).toBe(false);
    });

    it('gives the mouse back when the game shows its pointer again', () => {
        const exit = vi.fn();
        document.exitPointerLock = exit;
        Object.defineProperty(document, 'pointerLockElement', {
            configurable: true,
            get: () => v.inputEl,
        });
        v.pointerLocked = true;
        v._hostDrawsCursor = true;
        v._hostCursorVisible = true;
        v._onHostCursorVisibility();
        expect(exit).toHaveBeenCalled();
        delete document.pointerLockElement;
    });

    it('asks again on the next key when the browser refuses the lock', async () => {
        v._lockPointer = vi.fn(() => Promise.reject(new Error('no gesture')));
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._onHostCursorVisibility();
        key('w');
        await Promise.resolve();
        await Promise.resolve();
        expect(v._autoCaptureArmed).toBe(true);
    });

    it('stays out of gaming mode, which captures on its own terms', () => {
        v = makeView({ gaming: true });
        hostHidesPointer(v, 1000);
        pointerMoving(v);
        v._onHostCursorVisibility();
        expect(v._autoCaptureArmed).toBe(false);
    });
});

describe('the mouse once captured in desktop mode', () => {
    it('goes out as deltas, which have no edge to stop at', () => {
        const v = makeView();
        const sent = [];
        Object.assign(v, {
            pointerLocked: true,
            _rawPointer: false,
            _lockUnadjusted: true,
            _mouseSensitivity: 1,
            _mouseCarryX: 0,
            _mouseCarryY: 0,
            _sendToHost: (msg) => sent.push(msg),
        });
        v._setupNormalMouse();

        // A full turn is a long stroke in one direction: every count arrives.
        // jsdom's MouseEvent has no movementX/Y: give each move its own.
        const move = (dx, dy) => {
            const e = new MouseEvent('mousemove');
            Object.defineProperty(e, 'movementX', { value: dx });
            Object.defineProperty(e, 'movementY', { value: dy });
            v.inputEl.dispatchEvent(e);
        };
        for (let i = 0; i < 50; i++) move(40, 0);
        expect(sent).toHaveLength(50);
        expect(sent.every((m) => m.type === 'mousemove' && m.dx === 40 && m.dy === 0)).toBe(true);
        expect(sent.reduce((a, m) => a + m.dx, 0)).toBe(2000);
    });
});
