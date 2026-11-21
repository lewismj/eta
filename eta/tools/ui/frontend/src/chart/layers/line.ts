/**
 * Lines: one instanced capsule per segment, drawn in pixel space with an
 * analytic distance field for anti-aliasing (round joins and caps for free).
 *
 * Transitions: when the new series has the same number of vertices, every
 * vertex moves from where it is now; otherwise the old line fades out while
 * the new one fades in.
 */

import * as THREE from 'three/webgpu';
import {
  Fn, attribute, clamp, dot, float, length, max, mix, positionGeometry, select, smoothstep, uniform, varying, vec2, vec3,
} from 'three/tsl';
import { CATEGORICAL } from '../color';
import { FrameUniforms, InstanceBuffer, clipToRect, dataToPixel, flatMaterial, quadGeometry } from '../gpu';
import type { LineMark } from '../marks';
import { lerp } from '../scale';
import { interleave, planLine } from '../geometry';

export class LineLayer {
  readonly mesh: THREE.Mesh;
  private readonly geometry = quadGeometry();
  private readonly material = flatMaterial();
  private readonly aA: InstanceBuffer;
  private readonly aB: InstanceBuffer;
  private readonly aAlpha: InstanceBuffer;
  private readonly uWidth = uniform(2);
  private readonly uColor = uniform(new THREE.Color());
  private slot = 0;
  /** Vertex positions at the start and end of the current transition. */
  private vFrom: Float32Array | null = null;
  private vTo: Float32Array | null = null;
  count = 0;

  constructor(u: FrameUniforms) {
    const g = this.geometry;
    this.aA = new InstanceBuffer(g, 'aA', 4);
    this.aB = new InstanceBuffer(g, 'aB', 4);
    this.aAlpha = new InstanceBuffer(g, 'aAlpha', 2);

    const t = u.uT;
    const pa = attribute('aA', 'vec4');
    const pb = attribute('aB', 'vec4');
    const A = dataToPixel(u, mix(pa.xy, pa.zw, t));
    const B = dataToPixel(u, mix(pb.xy, pb.zw, t));
    const d = B.sub(A);
    const len = length(d);
    const dir = select(len.greaterThan(0.0001), d.div(max(len, 0.0001)), vec2(1, 0));
    const nrm = vec2(dir.y.negate(), dir.x);
    const hw = this.uWidth.mul(0.5).add(1.5);
    const gx = positionGeometry.x;
    const along = gx.add(0.5).mul(len).add(gx.mul(2).mul(hw));
    const across = positionGeometry.y.mul(2).mul(hw);
    const pixel = A.add(dir.mul(along)).add(nrm.mul(across));
    this.material.positionNode = vec3(pixel, 0);

    const vPixel = varying(pixel);
    const vA = varying(A);
    const vB = varying(B);
    const ap = vPixel.sub(vA);
    const ab = vB.sub(vA);
    const h = clamp(dot(ap, ab).div(max(dot(ab, ab), 0.000001)), 0, 1);
    const dist = length(ap.sub(ab.mul(h)));
    const half = this.uWidth.mul(0.5);
    const cover = float(1).sub(smoothstep(half.sub(0.6), half.add(0.6), dist));
    const fade = mix(attribute('aAlpha', 'vec2').x, attribute('aAlpha', 'vec2').y, t);

    this.material.colorNode = Fn(() => {
      clipToRect(u, vPixel);
      return this.uColor;
    })();
    this.material.opacityNode = cover.mul(fade);

    this.mesh = new THREE.Mesh(this.geometry, this.material);
    this.mesh.frustumCulled = false;
    this.mesh.renderOrder = 2;
  }

  refreshColors(theme: 'dark' | 'light'): void {
    this.uColor.value.set(CATEGORICAL[theme][this.slot]);
  }

  /** Vertex positions at eased time `t` (null before the first update). */
  verticesAt(t: number): Float32Array | null {
    if (!this.vTo) return null;
    if (!this.vFrom || this.vFrom.length !== this.vTo.length) return this.vTo;
    const out = new Float32Array(this.vTo.length);
    for (let i = 0; i < out.length; i++) out[i] = lerp(this.vFrom[i], this.vTo[i], t);
    return out;
  }

  update(m: LineMark, t: number, theme: 'dark' | 'light'): void {
    this.slot = m.slot;
    this.uWidth.value = m.emphasis ? m.width + 1 : m.width;
    const next = interleave(m.xs, m.ys);
    const now = this.verticesAt(t);
    const plan = planLine(now, next);
    const n = plan.alpha.length / 2;
    this.aA.ensure(n);
    this.aB.ensure(n);
    this.aAlpha.ensure(n);
    this.aA.data.set(plan.a);
    this.aB.data.set(plan.b);
    this.aAlpha.data.set(plan.alpha);
    this.count = n;
    this.geometry.instanceCount = n;
    this.aA.dirty();
    this.aB.dirty();
    this.aAlpha.dirty();
    this.vFrom = now && now.length === next.length ? now : next;
    this.vTo = next;
    this.refreshColors(theme);
  }

  dispose(): void {
    this.geometry.dispose();
    this.material.dispose();
  }
}
