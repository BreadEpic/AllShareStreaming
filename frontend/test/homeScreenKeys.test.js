/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A home-screen shortcut carries one key per machine inside a single `#k=`
 * value, and never sends a key twice. Both are pinned here: the first because
 * the bootstrap only relays `k=` as it is, the second because a key sent back
 * spent is a refusal that counts against the abuse guard.
 */

import { describe, it, expect, beforeEach } from 'vitest';
import {
    packHandoffKeys,
    unpackHandoffKeys,
    isHomeScreenHandoff,
    startUrlWithSideKeys,
    isHandoffKeySpent,
    markHandoffKeySpent,
} from '../js/util/homeScreenKeys.js';

const A = 'hs-AAAAAAAAAAAAAAAAAAAAAA';
const B = 'hs-BBBBBBBBBBBBBBBBBBBBBB';
const C = 'hs-CCCCCCCCCCCCCCCCCCCCCC';
const idB = '0123456789abcdefghjkmnpqrs';
const idC = 'zyxwvtsrqpnmkjhgfedcba9876';

describe('packHandoffKeys / unpackHandoffKeys', () => {
    it('round-trips the own key and the others', () => {
        const packed = packHandoffKeys(A, [
            { id: idB, key: B },
            { id: idC, key: C },
        ]);
        expect(packed).toBe(`${A},${idB}:${B},${idC}:${C}`);
        expect(unpackHandoffKeys(packed)).toEqual({
            own: A,
            others: [
                { id: idB, key: B },
                { id: idC, key: C },
            ],
        });
    });

    it('is the bare key when there is nobody else', () => {
        expect(packHandoffKeys(A, [])).toBe(A);
        expect(unpackHandoffKeys(A)).toEqual({ own: A, others: [] });
    });

    it('keeps the others when the page had no key of its own', () => {
        const packed = packHandoffKeys('', [{ id: idB, key: B }]);
        expect(packed).toBe(`,${idB}:${B}`);
        const parsed = unpackHandoffKeys(packed);
        expect(parsed).toEqual({ own: '', others: [{ id: idB, key: B }] });
        expect(isHomeScreenHandoff(parsed)).toBe(true);
    });

    it('leaves out what is not shaped like a machine and a key', () => {
        expect(
            packHandoffKeys(A, [{ id: 'short', key: B }, { id: idB, key: 'not-a-key' }, null]),
        ).toBe(A);
        expect(unpackHandoffKeys(`${A},garbage,${idB}:nope,${idB}:${B}`).others).toEqual([
            { id: idB, key: B },
        ]);
    });

    it('passes a host key through untouched', () => {
        const parsed = unpackHandoffKeys('mwk-something-else');
        expect(parsed).toEqual({ own: 'mwk-something-else', others: [] });
        expect(isHomeScreenHandoff(parsed)).toBe(false);
        expect(unpackHandoffKeys(null)).toEqual({ own: '', others: [] });
        expect(isHomeScreenHandoff(unpackHandoffKeys(''))).toBe(false);
    });
});

describe('startUrlWithSideKeys', () => {
    it('writes the others beside the key the manifest already carries', () => {
        const url = startUrlWithSideKeys(`/${idC}#k=${A}`, [{ id: idB, key: B }]);
        expect(url).toBe(`/${idC}#k=${encodeURIComponent(`${A},${idB}:${B}`)}`);
        expect(unpackHandoffKeys(decodeURIComponent(url.split('#k=')[1]))).toEqual({
            own: A,
            others: [{ id: idB, key: B }],
        });
    });

    it('adds a #k= when the manifest had none, and keeps the other parts', () => {
        expect(startUrlWithSideKeys(`/${idC}`, [{ id: idB, key: B }])).toBe(
            `/${idC}#k=${encodeURIComponent(`,${idB}:${B}`)}`,
        );
        expect(startUrlWithSideKeys(`/${idC}#p=%2Fadmin&k=${A}`, [{ id: idB, key: B }])).toBe(
            `/${idC}#p=%2Fadmin&k=${encodeURIComponent(`${A},${idB}:${B}`)}`,
        );
    });

    it('changes nothing when there is nothing to add', () => {
        expect(startUrlWithSideKeys(`/${idC}#k=${A}`, [])).toBe(`/${idC}#k=${A}`);
        expect(startUrlWithSideKeys(undefined, [{ id: idB, key: B }])).toBeUndefined();
    });
});

describe('spent keys', () => {
    beforeEach(() => localStorage.clear());

    it('remembers a key once it has had its answer', () => {
        expect(isHandoffKeySpent(A)).toBe(false);
        markHandoffKeySpent(A);
        expect(isHandoffKeySpent(A)).toBe(true);
        expect(isHandoffKeySpent(B)).toBe(false);
    });

    it('survives a broken record and an absent storage', () => {
        localStorage.setItem('mw-hs-spent', '{not json');
        expect(isHandoffKeySpent(A)).toBe(false);
        markHandoffKeySpent(A);
        expect(isHandoffKeySpent(A)).toBe(true);
        expect(isHandoffKeySpent(A, null)).toBe(false);
        expect(() => markHandoffKeySpent(A, null)).not.toThrow();
    });

    it('keeps the newest keys only', () => {
        for (let i = 0; i < 80; i++) markHandoffKeySpent(`hs-${String(i).padStart(20, '0')}`);
        expect(isHandoffKeySpent('hs-00000000000000000000')).toBe(false);
        expect(isHandoffKeySpent('hs-00000000000000000079')).toBe(true);
    });
});
