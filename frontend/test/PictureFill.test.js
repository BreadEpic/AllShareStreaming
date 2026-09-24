/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach } from 'vitest';
import {
    PICTURE_FILLS,
    loadPictureFill,
    nextPictureFill,
    normalizePictureFill,
    pictureRect,
    savePictureFill,
} from '../js/util/PictureFill.js';

// A window out of fullscreen is shorter than the host's 16:9 screen: 1920×969
// here, with a 1920×1080 picture. Where the picture lands decides both what
// the CSS draws and where a click goes on the host.
const WINDOW = { left: 0, top: 0, width: 1920, height: 969 };

describe('picture fill modes', () => {
    it('knows the three modes and falls back to fit', () => {
        expect(PICTURE_FILLS).toEqual(['fit', 'stretch', 'zoom']);
        expect(normalizePictureFill('zoom')).toBe('zoom');
        expect(normalizePictureFill('cover')).toBe('fit');
        expect(normalizePictureFill(undefined)).toBe('fit');
    });

    it('cycles fit → stretch → zoom → fit for the shortcut', () => {
        expect(nextPictureFill('fit')).toBe('stretch');
        expect(nextPictureFill('stretch')).toBe('zoom');
        expect(nextPictureFill('zoom')).toBe('fit');
        expect(nextPictureFill('nonsense')).toBe('stretch');
    });
});

describe('where the picture lands', () => {
    it('fit keeps the whole picture, with bars on the sides', () => {
        const r = pictureRect(WINDOW, 1920, 1080, 'fit');
        expect(r.height).toBe(969);
        expect(r.width).toBeCloseTo(1722.67, 1);
        expect(r.left).toBeCloseTo(98.67, 1);
        expect(r.top).toBe(0);
    });

    it('stretch is the window itself: no bars, nothing cut', () => {
        expect(pictureRect(WINDOW, 1920, 1080, 'stretch')).toEqual(WINDOW);
    });

    it('zoom covers the window and overflows it top and bottom', () => {
        const r = pictureRect(WINDOW, 1920, 1080, 'zoom');
        expect(r.width).toBe(1920);
        expect(r.height).toBe(1080);
        expect(r.left).toBe(0);
        expect(r.top).toBeCloseTo(-55.5, 5);
    });

    it('leaves the box alone while the picture size is unknown', () => {
        expect(pictureRect(WINDOW, 0, 0, 'zoom')).toBe(WINDOW);
    });
});

describe('the saved mode', () => {
    afterEach(() => localStorage.clear());

    it('is fit when nothing was saved', () => {
        expect(loadPictureFill()).toBe('fit');
    });

    it('is saved with the other streaming settings, which it leaves untouched', () => {
        localStorage.setItem(
            'mw-streaming-settings',
            JSON.stringify({ video_codec: 'av1', gaming_mode: true }),
        );
        savePictureFill('stretch');

        expect(loadPictureFill()).toBe('stretch');
        expect(JSON.parse(localStorage.getItem('mw-streaming-settings'))).toEqual({
            video_codec: 'av1',
            gaming_mode: true,
            picture_fill: 'stretch',
        });
    });

    it('survives a damaged settings entry', () => {
        localStorage.setItem('mw-streaming-settings', '{not json');
        expect(loadPictureFill()).toBe('fit');
        savePictureFill('zoom');
        expect(loadPictureFill()).toBe('fit');
    });
});
