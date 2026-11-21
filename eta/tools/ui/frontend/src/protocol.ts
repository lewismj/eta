/**
 * Eta UI protocol over WebSocket.
 *
 * Server -> client frames (binary):
 *   [u32 total][u32 jsonLen][json (space padded)][buffers]
 * `total` counts the bytes after itself. Buffers start 8-byte aligned
 * relative to the frame start; the JSON header is
 *   { "buffers": [{offset, length, dtype}], "body": <message> }
 * and `{"$buf": i}` inside the body refers to buffers[i].
 *
 * Client -> server: [u32 jsonLen][json].
 *
 * NNG's stream mode may split or join WebSocket messages, so both directions
 * are treated as byte streams with explicit lengths.
 */

export type Status = 'connecting' | 'live' | 'reconnecting';

export interface Message {
  type: string;
  [key: string]: unknown;
}

interface BufferInfo {
  offset: number;
  length: number;
  dtype: 'f64';
}

const decoder = new TextDecoder();
const encoder = new TextEncoder();

function resolveBuffers(value: unknown, buffers: Float64Array[]): unknown {
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i++) value[i] = resolveBuffers(value[i], buffers);
    return value;
  }
  if (value !== null && typeof value === 'object') {
    const obj = value as Record<string, unknown>;
    if (typeof obj.$buf === 'number' && Object.keys(obj).length === 1) {
      return buffers[obj.$buf];
    }
    for (const k of Object.keys(obj)) obj[k] = resolveBuffers(obj[k], buffers);
    return obj;
  }
  return value;
}

/** Decode one complete frame (starting with its `total` field). */
export function decodeFrame(frame: ArrayBuffer): Message {
  const view = new DataView(frame);
  const jsonLen = view.getUint32(4, true);
  const header = JSON.parse(decoder.decode(new Uint8Array(frame, 8, jsonLen))) as {
    buffers: BufferInfo[];
    body: Message;
  };
  const base = 8 + jsonLen;
  const buffers = header.buffers.map(
    (b) => new Float64Array(frame, base + b.offset, b.length),
  );
  return resolveBuffers(header.body, buffers) as Message;
}

/**
 * Reassembles length-prefixed frames from a byte stream. Each returned frame
 * is a fresh ArrayBuffer (starting with its `total` field), so Float64Array
 * views into it are aligned.
 */
export class FrameReader {
  private pending = new Uint8Array(0);

  push(chunk: Uint8Array): ArrayBuffer[] {
    let buf: Uint8Array;
    if (this.pending.length === 0) {
      buf = chunk;
    } else {
      buf = new Uint8Array(this.pending.length + chunk.length);
      buf.set(this.pending, 0);
      buf.set(chunk, this.pending.length);
    }
    const frames: ArrayBuffer[] = [];
    let pos = 0;
    while (buf.length - pos >= 4) {
      const total = new DataView(buf.buffer, buf.byteOffset + pos, 4).getUint32(0, true);
      if (buf.length - pos - 4 < total) break;
      frames.push(buf.slice(pos, pos + 4 + total).buffer);
      pos += 4 + total;
    }
    this.pending = pos === buf.length ? new Uint8Array(0) : buf.slice(pos);
    return frames;
  }

  get buffered(): number {
    return this.pending.length;
  }
}

/** Encode a client -> server message. */
export function encodeClientMessage(msg: Message): Uint8Array {
  const json = encoder.encode(JSON.stringify(msg));
  const out = new Uint8Array(4 + json.length);
  new DataView(out.buffer).setUint32(0, json.length, true);
  out.set(json, 4);
  return out;
}

export interface ConnectionOptions {
  onMessage: (msg: Message) => void;
  onStatus: (status: Status) => void;
}

export class Connection {
  private ws: WebSocket | null = null;
  private reader = new FrameReader();
  private retryMs = 300;
  private closed = false;
  private everConnected = false;

  constructor(private readonly url: string, private readonly opts: ConnectionOptions) {
    this.open();
  }

  static fromLocation(opts: ConnectionOptions): Connection {
    const params = new URLSearchParams(location.search);
    const token = params.get('t') ?? '';
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    return new Connection(`${scheme}://${location.host}/ws?t=${encodeURIComponent(token)}`, opts);
  }

  send(msg: Message): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(encodeClientMessage(msg));
  }

  close(): void {
    this.closed = true;
    this.ws?.close();
  }

  private open(): void {
    this.opts.onStatus(this.everConnected ? 'reconnecting' : 'connecting');
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    this.reader = new FrameReader();

    ws.onopen = () => {
      this.everConnected = true;
      this.retryMs = 300;
      this.opts.onStatus('live');
    };
    ws.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) this.feed(new Uint8Array(ev.data));
    };
    ws.onclose = () => {
      this.ws = null;
      if (this.closed) return;
      this.opts.onStatus(this.everConnected ? 'reconnecting' : 'connecting');
      setTimeout(() => this.open(), this.retryMs);
      this.retryMs = Math.min(this.retryMs * 1.5, 2000);
    };
    ws.onerror = () => ws.close();
  }

  private feed(chunk: Uint8Array): void {
    for (const frame of this.reader.push(chunk)) {
      try {
        this.opts.onMessage(decodeFrame(frame));
      } catch (err) {
        console.error('eta-ui: bad frame', err);
      }
    }
  }
}
