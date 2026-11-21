import { describe, expect, it } from 'vitest';
import { FrameReader, decodeFrame, encodeClientMessage } from './protocol';

/** Build a server frame exactly as the native side does (see ui_server.h). */
function serverFrame(body: unknown, buffers: number[][] = []): Uint8Array {
  const index: { offset: number; length: number; dtype: 'f64' }[] = [];
  const parts: Float64Array[] = [];
  let offset = 0;
  for (const b of buffers) {
    index.push({ offset, length: b.length, dtype: 'f64' });
    parts.push(Float64Array.from(b));
    offset += b.length * 8;
  }
  let json = JSON.stringify({ buffers: index, body });
  while ((8 + json.length) % 8 !== 0) json += ' ';
  const jsonBytes = new TextEncoder().encode(json);
  const out = new Uint8Array(8 + jsonBytes.length + offset);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, 4 + jsonBytes.length + offset, true);
  dv.setUint32(4, jsonBytes.length, true);
  out.set(jsonBytes, 8);
  let pos = 8 + jsonBytes.length;
  for (const p of parts) {
    out.set(new Uint8Array(p.buffer), pos);
    pos += p.byteLength;
  }
  return out;
}

describe('decodeFrame', () => {
  it('decodes the JSON body', () => {
    const msg = decodeFrame(serverFrame({ type: 'render', rev: 3 }).slice().buffer);
    expect(msg).toEqual({ type: 'render', rev: 3 });
  });

  it('resolves $buf references to Float64Array views, at any depth', () => {
    const frame = serverFrame(
      { type: 'render', tree: { p: { xs: { $buf: 0 } }, c: [{ p: { ys: { $buf: 1 } } }] } },
      [[1.5, 2.5, 3.5], [10, 20]],
    );
    const msg = decodeFrame(frame.slice().buffer) as any;
    expect(msg.tree.p.xs).toBeInstanceOf(Float64Array);
    expect(Array.from(msg.tree.p.xs)).toEqual([1.5, 2.5, 3.5]);
    expect(Array.from(msg.tree.c[0].p.ys)).toEqual([10, 20]);
  });

  it('leaves ordinary objects with a $buf field plus others untouched', () => {
    const msg = decodeFrame(serverFrame({ x: { $buf: 0, other: 1 } }, [[1]]).slice().buffer) as any;
    expect(msg.x).toEqual({ $buf: 0, other: 1 });
  });
});

describe('FrameReader', () => {
  const a = serverFrame({ type: 'a' });
  const b = serverFrame({ type: 'b' }, [[1, 2, 3]]);

  it('returns whole frames from one chunk', () => {
    const r = new FrameReader();
    const both = new Uint8Array(a.length + b.length);
    both.set(a, 0);
    both.set(b, a.length);
    const frames = r.push(both);
    expect(frames.map((f) => decodeFrame(f).type)).toEqual(['a', 'b']);
    expect(r.buffered).toBe(0);
  });

  it('reassembles frames split at every byte boundary', () => {
    for (let cut = 1; cut < b.length; cut++) {
      const r = new FrameReader();
      expect(r.push(b.slice(0, cut))).toHaveLength(0);
      const frames = r.push(b.slice(cut));
      expect(frames).toHaveLength(1);
      const msg = decodeFrame(frames[0]) as any;
      expect(msg.type).toBe('b');
    }
  });

  it('keeps buffers aligned when frames arrive at odd offsets', () => {
    const r = new FrameReader();
    const junkPrefix = serverFrame({ type: 'x' });
    const stream = new Uint8Array(junkPrefix.length + b.length);
    stream.set(junkPrefix, 0);
    stream.set(b, junkPrefix.length);
    // Deliver from an odd offset of a larger buffer.
    const host = new Uint8Array(stream.length + 3);
    host.set(stream, 3);
    const frames = r.push(host.subarray(3));
    expect(frames).toHaveLength(2);
    const msg = decodeFrame(frames[1]) as any;
    expect(msg.type).toBe('b');
  });
});

describe('encodeClientMessage', () => {
  it('prefixes the JSON with its little-endian length', () => {
    const bytes = encodeClientMessage({ type: 'event', h: 'r/0#on-click', data: { index: 2 } });
    const len = new DataView(bytes.buffer).getUint32(0, true);
    expect(len).toBe(bytes.length - 4);
    const json = JSON.parse(new TextDecoder().decode(bytes.subarray(4)));
    expect(json).toEqual({ type: 'event', h: 'r/0#on-click', data: { index: 2 } });
  });
});
