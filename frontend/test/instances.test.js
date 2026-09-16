/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */

import { describe, it, expect, beforeEach } from 'vitest';
import { importInstances, listInstances, rememberInstance } from '../js/util/instances.js';

// A home-screen shortcut starts with an empty register and receives the
// browser's list once, on its first start.
describe('importInstances', () => {
    beforeEach(() => localStorage.clear());

    it('fills an empty register', () => {
        importInstances([
            { id: 'a', name: 'DualRTX', url: 'https://x/a' },
            { id: 'b', name: 'N95', url: 'https://x/b' },
        ]);
        expect(listInstances().map((e) => e.name)).toEqual(['DualRTX', 'N95']);
    });

    it('never overrides what this register already knows', () => {
        rememberInstance({ id: 'a', name: 'Renamed', url: 'https://x/a' });
        importInstances([{ id: 'a', name: 'Old name', url: 'https://x/a' }]);
        expect(listInstances()).toHaveLength(1);
        expect(listInstances()[0].name).toBe('Renamed');
    });

    it('drops what it cannot read', () => {
        importInstances([null, { id: 'a' }, { id: 'b', name: 'N95', url: '' }, 'x']);
        expect(listInstances()).toEqual([]);
        importInstances('not a list');
        expect(listInstances()).toEqual([]);
    });
});
