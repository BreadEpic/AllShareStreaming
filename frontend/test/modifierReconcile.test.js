/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * A modifier whose key-up the local OS ate stays down on the host.
 *
 * Press the Windows key and the Start menu takes the focus; the browser sees
 * the keydown and never the keyup. Blur catches most of it, not all. On a
 * macOS host the damage then outlives the slip, because there a modifier is
 * not a key the OS holds for us but a flag the host puts on every event it
 * injects: Command left down turned the Space bar into Cmd+Space, and
 * Spotlight opened instead of the character jumping.
 *
 * Every keyboard event states the whole modifier state, so the truth is on
 * each one of them. These tests pin that the client reads it and releases
 * what it wrongly believes is held — and that it does not touch a chord the
 * viewer is genuinely holding.
 */

const VK_SPACE = 0x20;
const VK_W = 0x57;
const VK_LWIN = 0x5b;
const VK_LCTRL = 0x11;

/** Minimal stand-in exposing just what the keyboard path touches. */
function keySink(overrides = {}) {
    const sent = [];
    return {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        // A PC keyboard: none of the Apple Cmd workarounds are in play.
        _appleKeyboard: false,
        _quitting: false,
        _pendingClipboardWrite: null,
        _kbdCapture: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        handleKeyDown: StreamView.prototype.handleKeyDown,
        handleKeyUp: StreamView.prototype.handleKeyUp,
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _reconcileModifiers: StreamView.prototype._reconcileModifiers,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _releaseHeldMeta: StreamView.prototype._releaseHeldMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        ...overrides,
    };
}

/** A KeyboardEvent-shaped plain object, with no getModifierState: the shape
 *  the older browsers and the soft keyboard's own events have. */
function ev(code, key, mods = {}) {
    return {
        target: {},
        code,
        key,
        keyCode: 0,
        repeat: false,
        ctrlKey: false,
        shiftKey: false,
        altKey: false,
        metaKey: false,
        preventDefault() {},
        ...mods,
    };
}

/** The same, with the normative getModifierState a real browser provides. */
function evState(code, key, held = [], mods = {}) {
    const e = ev(code, key, mods);
    e.getModifierState = (name) => held.includes(name);
    return e;
}

/** The Windows key going down and its key-up never arriving. */
function stickMeta(v) {
    v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
    v.sent.length = 0;
}

describe('a modifier the local OS stopped talking about', () => {
    it('is released before the keystroke that gives it away', () => {
        const v = keySink();
        stickMeta(v);

        v.handleKeyDown(ev('Space', ' '));

        // The release goes out FIRST: the host must not see this Space with
        // Command still on it, which is the whole bug.
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keydown', VK_SPACE],
        ]);
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(false);
    });

    it('is cleared by a repeating movement key, with nothing asked of the player', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyW', 'w'));
        stickMeta(v);

        // OS auto-repeat of the key already held: the branch that replays it
        // sits BELOW the reconciliation on purpose.
        v.handleKeyDown(ev('KeyW', 'w', { repeat: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keydown', VK_W],
        ]);
    });

    it('is released on the way up too, above the branches that return early', () => {
        const v = keySink();
        v.handleKeyDown(ev('Space', ' '));
        stickMeta(v);

        v.handleKeyUp(ev('Space', ' '));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keyup', VK_SPACE],
        ]);
    });

    it('takes getModifierState over the event flags when the browser has it', () => {
        const v = keySink();
        v.handleKeyDown(evState('ControlLeft', 'Control', ['Control'], { ctrlKey: true }));
        v.sent.length = 0;

        // The flags still say Control — getModifierState is the truth.
        v.handleKeyDown(evState('Space', ' ', [], { ctrlKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LCTRL],
            ['keydown', VK_SPACE],
        ]);
    });

    it('leaves a chord the viewer is really holding alone', () => {
        const v = keySink();
        stickMeta(v);

        v.handleKeyDown(ev('Space', ' ', { metaKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(true);
    });

    it('does not release the modifier whose own event this is', () => {
        const v = keySink();
        stickMeta(v);

        // Meta finally coming up: one key-up, not two.
        v.handleKeyUp(ev('MetaLeft', 'Meta'));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keyup', VK_LWIN]]);
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('never speaks for Caps Lock, which is a lock and not a held key', () => {
        const v = keySink();
        v.handleKeyDown(ev('CapsLock', 'CapsLock'));
        v.sent.length = 0;

        v.handleKeyDown(evState('Space', ' ', ['CapsLock']));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has('CapsLock')).toBe(true);
    });

    it('leaves a toolbar-latched modifier latched', () => {
        const v = keySink();
        // The soft keyboard latches a modifier with no physical key behind it,
        // so _sendKeyEvent files it under its numeric id.
        v._sendKeyEvent.call(v, { type: 'keydown', keyCode: VK_LWIN, code: '', key: 'Meta' });
        v.sent.length = 0;

        v.handleKeyDown(ev('Space', ' '));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has(VK_LWIN)).toBe(true);
    });

    it('does not arm a swallow token, which would eat the next real release', () => {
        const v = keySink();
        stickMeta(v);
        v.handleKeyDown(ev('Space', ' '));
        expect(v._swallowKeyUpCodes.size).toBe(0);

        // The viewer presses and releases the Windows key for real: both edges
        // reach the host.
        v.sent.length = 0;
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
        v.handleKeyUp(ev('MetaLeft', 'Meta'));
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_LWIN],
            ['keyup', VK_LWIN],
        ]);
    });
});

/** Minimal stand-in for the held-input heartbeat. */
function beatSink(overrides = {}) {
    const sent = [];
    return {
        sent,
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _gamepadManager: null,
        _gamingMode: false,
        _lastBeatHadState: false,
        _sendInputState: StreamView.prototype._sendInputState,
        _sendToHost: (m) => sent.push(m),
        ...overrides,
    };
}

describe('the closing beat of the held-input heartbeat', () => {
    it('says once that nothing is held, then goes quiet', () => {
        const v = beatSink();
        v._heldPhysKeys.set('KeyW', { keyCode: VK_W, code: 'KeyW', key: 'w' });
        v._sendInputState();
        expect(v.sent.length).toBe(1);
        expect(v.sent[0].keys.length).toBe(1);

        // The key comes up, and the next beat carries the empty state — this is
        // what makes the host let go of anything it still believes is held.
        v._heldPhysKeys.clear();
        v._sendInputState();
        expect(v.sent.length).toBe(2);
        expect(v.sent[1]).toMatchObject({ type: 'inputstate', keys: [], buttons: 0 });

        // And then nothing, beat after beat.
        v._sendInputState();
        v._sendInputState();
        expect(v.sent.length).toBe(2);
    });

    it('still says nothing at all when nothing was ever held', () => {
        const v = beatSink();
        v._sendInputState();
        v._sendInputState();
        expect(v.sent).toEqual([]);
    });

    it('keeps the forced opening beat, which arms the host watchdog', () => {
        const v = beatSink();
        v._sendInputState(true);
        expect(v.sent.length).toBe(1);
        expect(v.sent[0].keys).toEqual([]);
        // A forced beat with nothing held must not then owe a closing one.
        v._sendInputState();
        expect(v.sent.length).toBe(1);
    });
});
