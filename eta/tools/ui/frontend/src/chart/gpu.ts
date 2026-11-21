/**
 * Shared TSL building blocks for chart layers.
 *
 * All marks are drawn in pixel space (orthographic camera over the canvas in
 * CSS pixels, y up). Data -> pixel mapping uses the engine's animated domain.
 */

import * as THREE from 'three/webgpu';
import { Discard, If, mix, or, uniform, vec2 } from 'three/tsl';

// TSL node types are deeply generic; layers treat nodes structurally.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type N = any;

/** Uniforms every layer reads. Owned by the engine. */
export interface FrameUniforms {
  /** Eased data-transition time (positions, opacity, selection). */
  uT: N;
  /** Eased domain-transition time (zoom/pan and rescaling). */
  uDomT: N;
  uDomFrom: N;
  uDomTo: N;
  /** Plot rectangle in pixels: (left, bottom, width, height). */
  uRect: N;
}

export function makeFrameUniforms(): FrameUniforms {
  return {
    uT: uniform(1),
    uDomT: uniform(1),
    uDomFrom: uniform(new THREE.Vector4(0, 1, 0, 1)),
    uDomTo: uniform(new THREE.Vector4(0, 1, 0, 1)),
    uRect: uniform(new THREE.Vector4(0, 0, 1, 1)),
  };
}

/** Map a data-space vec2 node to pixels with the current animated domain. */
export function dataToPixel(u: FrameUniforms, p: N): N {
  const dom = mix(u.uDomFrom, u.uDomTo, u.uDomT);
  const nx = p.x.sub(dom.x).div(dom.y.sub(dom.x));
  const ny = p.y.sub(dom.z).div(dom.w.sub(dom.z));
  return vec2(u.uRect.x.add(nx.mul(u.uRect.z)), u.uRect.y.add(ny.mul(u.uRect.w)));
}

/**
 * Discard fragments outside the plot rectangle (`pixel` is a varying holding
 * the fragment's pixel position). Used instead of the scissor test, whose y
 * origin differs between the WebGPU and WebGL backends.
 */
export function clipToRect(u: FrameUniforms, pixel: N, pad = 0): void {
  const r = u.uRect;
  If(
    or(
      or(pixel.x.lessThan(r.x.sub(pad)), pixel.x.greaterThan(r.x.add(r.z).add(pad))),
      or(pixel.y.lessThan(r.y.sub(pad)), pixel.y.greaterThan(r.y.add(r.w).add(pad))),
    ),
    () => {
      Discard();
    },
  );
}

export function cssColor(name: string, fallback: string): THREE.Color {
  if (typeof document === 'undefined') return new THREE.Color(fallback);
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return new THREE.Color(v || fallback);
}

/** A transparent, depth-free basic node material (2D overlay drawing). */
export function flatMaterial(): THREE.MeshBasicNodeMaterial {
  const m = new THREE.MeshBasicNodeMaterial();
  m.transparent = true;
  m.depthWrite = false;
  m.depthTest = false;
  return m;
}

/** Instanced unit quad (positions in [-0.5, 0.5], uv in [0, 1]). */
export function quadGeometry(): THREE.InstancedBufferGeometry {
  const quad = new THREE.PlaneGeometry(1, 1);
  const g = new THREE.InstancedBufferGeometry();
  g.setIndex(quad.getIndex());
  g.setAttribute('position', quad.getAttribute('position'));
  g.setAttribute('uv', quad.getAttribute('uv'));
  g.instanceCount = 0;
  return g;
}

/** Growable typed attribute storage for instanced layers. */
export class InstanceBuffer {
  data: Float32Array;
  constructor(
    readonly geometry: THREE.InstancedBufferGeometry,
    readonly name: string,
    readonly itemSize: number,
    capacity = 64,
  ) {
    this.data = new Float32Array(capacity * itemSize);
    this.attach();
  }

  ensure(count: number): void {
    if (count * this.itemSize <= this.data.length) return;
    let cap = Math.max(64, this.data.length / this.itemSize);
    while (cap < count) cap *= 2;
    const next = new Float32Array(cap * this.itemSize);
    next.set(this.data);
    this.data = next;
    this.attach();
  }

  dirty(): void {
    (this.geometry.getAttribute(this.name) as THREE.InstancedBufferAttribute).needsUpdate = true;
  }

  private attach(): void {
    this.geometry.setAttribute(this.name, new THREE.InstancedBufferAttribute(this.data, this.itemSize));
  }
}
