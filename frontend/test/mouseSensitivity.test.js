/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * The Settings mouse sensitivity scales relative motion (pointer lock) before
 * it leaves the browser. The host only ever sees whole counts, so the scaled
 * fractions are carried from one move to the next: at 0.5 a slow hand is half
 * as fast, not stopped.
 */
function view(sensitivity) {
    const sent = [];
    const v = {
        _mouseSensitivity: sensitivity,
        _mouseCarryX: 0,
        _mouseCarryY: 0,
        _sendToHost: (msg) => sent.push(msg),
    };
    const move = (dx, dy) => StreamView.prototype._sendRelativeMouse.call(v, dx, dy);
    return { sent, move };
}

describe('mouse sensitivity', () => {
    it('sends the counts untouched at 1', () => {
        const { sent, move } = view(1);
        move(3, -2);
        expect(sent).toEqual([{ type: 'mousemove', dx: 3, dy: -2 }]);
    });

    it('scales up', () => {
        const { sent, move } = view(2);
        move(3, -2);
        expect(sent).toEqual([{ type: 'mousemove', dx: 6, dy: -4 }]);
    });

    it('keeps the fractions below 1: two one-count moves at 0.5 are one count', () => {
        const { sent, move } = view(0.5);
        move(1, 0);
        expect(sent).toEqual([]);
        move(1, 0);
        expect(sent).toEqual([{ type: 'mousemove', dx: 1, dy: 0 }]);
    });

    it('loses nothing over a long slow stroke, in either direction', () => {
        const { sent, move } = view(1.3);
        for (let i = 0; i < 100; i++) move(1, -1);
        const dx = sent.reduce((a, m) => a + m.dx, 0);
        const dy = sent.reduce((a, m) => a + m.dy, 0);
        expect(dx).toBe(130);
        expect(dy).toBe(-130);
    });
});
