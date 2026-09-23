// Injected into the client page over DevTools once a stream is up (cadence.py).
// Records, while window.__mwc.on is true:
//   raf   every refresh of this window's screen (requestAnimationFrame stamps)
//   dec   [performance.now(), chunk timestamp] per frame handed to the decoder
//   draw  [performance.now(), frame timestamp] per VideoFrame drawn on a 2D canvas
// The chunk/frame timestamp is the host's capture time (backendTs, ms) × 1000,
// so the host's cadence is read in its own clock and the client's in its own.
// Prototype patches, applied to a running page: nothing in the app changes, and
// a page this was never injected into pays nothing.
(() => {
    if (window.__mwc) return 'already';
    const C = (window.__mwc = { raf: [], dec: [], draw: [], other: 0, on: false });
    const drawImage = CanvasRenderingContext2D.prototype.drawImage;
    CanvasRenderingContext2D.prototype.drawImage = function (img, ...rest) {
        if (C.on) {
            if (img instanceof VideoFrame) C.draw.push(performance.now(), img.timestamp);
            else C.other++;
        }
        return drawImage.call(this, img, ...rest);
    };
    const decode = VideoDecoder.prototype.decode;
    VideoDecoder.prototype.decode = function (chunk) {
        if (C.on) C.dec.push(performance.now(), chunk.timestamp);
        return decode.call(this, chunk);
    };
    const tick = (t) => {
        if (C.on) C.raf.push(t);
        requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return 'hooked';
})();
