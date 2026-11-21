/**
 * Horizontal bars on a band (category) axis. Each bar is an instanced quad
 * shaded with a rounded-box distance field: 4px radius at the value end,
 * square at the baseline. Bars are keyed by category, so re-sorting slides
 * them to their new rows and value changes animate their length.
 */

import * as THREE from 'three/webgpu';
import {
  Fn, abs, attribute, float, length, max, min, mix, positionGeometry, select, smoothstep, uniform, varying, vec2, vec3,
} from 'three/tsl';
import { CATEGORICAL } from '../color';
import { FrameUniforms, InstanceBuffer, clipToRect, dataToPixel, flatMaterial, quadGeometry } from '../gpu';
import type { BarsMark } from '../marks';
import { lerp } from '../scale';
import { BarNow, planBars } from '../geometry';

/** Fraction of each band left empty between bars. */
export const BAR_PADDING = 0.3;

export class BarsLayer {
  readonly mesh: THREE.Mesh;
  private readonly geometry = quadGeometry();
  private readonly material = flatMaterial();
  private readonly center: InstanceBuffer; // (c0, c1)
  private readonly span: InstanceBuffer; // (v0 from, v1 from, v0 to, v1 to)
  private readonly alpha: InstanceBuffer; // (a0, a1)
  private readonly sel: InstanceBuffer; // (s0, s1)
  private readonly hover: InstanceBuffer;
  private readonly uColor = uniform(new THREE.Color());
  private readonly uDim = uniform(0);
  private slot = 0;
  categories: string[] = [];
  dataIndex: number[] = [];
  count = 0;
  private hoverInstance = -1;

  constructor(u: FrameUniforms) {
    const g = this.geometry;
    this.center = new InstanceBuffer(g, 'aCenter', 2);
    this.span = new InstanceBuffer(g, 'aSpan', 4);
    this.alpha = new InstanceBuffer(g, 'aAlpha', 2);
    this.sel = new InstanceBuffer(g, 'aSel', 2);
    this.hover = new InstanceBuffer(g, 'aHover', 1);

    const t = u.uT;
    const c = mix(attribute('aCenter', 'vec2').x, attribute('aCenter', 'vec2').y, t);
    const sp = attribute('aSpan', 'vec4');
    const v0 = mix(sp.x, sp.z, t);
    const v1 = mix(sp.y, sp.w, t);
    const half = float((1 - BAR_PADDING) / 2);
    const p0 = dataToPixel(u, vec2(v0, c.sub(half)));
    const p1 = dataToPixel(u, vec2(v1, c.add(half)));
    const base = dataToPixel(u, vec2(0, c)).x;
    const bmin = min(p0, p1);
    const bmax = max(p0, p1);
    const pad = float(2);
    const pixel = mix(bmin.sub(pad), bmax.add(pad), positionGeometry.xy.add(0.5));
    this.material.positionNode = vec3(pixel, 0);

    const vPixel = varying(pixel);
    const vMin = varying(bmin);
    const vMax = varying(bmax);
    const vBase = varying(base);
    // Extend the box past the baseline by the radius so that end stays square.
    const halfSize = vMax.sub(vMin).mul(0.5);
    const r = min(float(4), min(halfSize.x, halfSize.y));
    const anchoredLeft = abs(vMin.x.sub(vBase)).lessThan(abs(vMax.x.sub(vBase)));
    const lo = vec2(select(anchoredLeft, vMin.x.sub(r), vMin.x), vMin.y);
    const hi = vec2(select(anchoredLeft, vMax.x, vMax.x.add(r)), vMax.y);
    const cpx = lo.add(hi).mul(0.5);
    const hsz = hi.sub(lo).mul(0.5);
    const q = abs(vPixel.sub(cpx)).sub(hsz.sub(r));
    const sdf = length(max(q, 0)).add(min(max(q.x, q.y), 0)).sub(r);
    const cover = float(1).sub(smoothstep(-0.6, 0.6, sdf));
    // Never draw on the far side of the baseline.
    const beyond = select(anchoredLeft, vPixel.x.lessThan(vBase), vPixel.x.greaterThan(vBase));
    const inside = select(beyond, float(0), cover);

    const s = mix(attribute('aSel', 'vec2').x, attribute('aSel', 'vec2').y, t);
    const hov = attribute('aHover', 'float');
    const fade = mix(attribute('aAlpha', 'vec2').x, attribute('aAlpha', 'vec2').y, t);
    // When something is selected, other bars recede.
    const emphasis = mix(float(1), mix(float(0.45), float(1), s), this.uDim);
    const color = mix(this.uColor, vec3(1, 1, 1), hov.mul(0.12).add(s.mul(0.1)));

    this.material.colorNode = Fn(() => {
      clipToRect(u, vPixel);
      return color;
    })();
    this.material.opacityNode = inside.mul(fade).mul(emphasis);

    this.mesh = new THREE.Mesh(this.geometry, this.material);
    this.mesh.frustumCulled = false;
    this.mesh.renderOrder = 1;
  }

  refreshColors(theme: 'dark' | 'light'): void {
    this.uColor.value.set(CATEGORICAL[theme][this.slot]);
  }

  private nowAt(t: number): BarNow[] {
    const out: BarNow[] = [];
    const c = this.center.data, sp = this.span.data, a = this.alpha.data, s = this.sel.data;
    for (let i = 0; i < this.count; i++) {
      out.push({
        category: this.categories[i],
        center: lerp(c[2 * i], c[2 * i + 1], t),
        v0: lerp(sp[4 * i], sp[4 * i + 2], t),
        v1: lerp(sp[4 * i + 1], sp[4 * i + 3], t),
        alpha: lerp(a[2 * i], a[2 * i + 1], t),
        sel: lerp(s[2 * i], s[2 * i + 1], t),
      });
    }
    return out;
  }

  update(m: BarsMark, t: number, theme: 'dark' | 'light'): void {
    this.slot = m.slot;
    const plan = planBars(this.nowAt(t), m.categories, m.values, m.selected ?? null);
    for (const b of [this.center, this.span, this.alpha, this.sel, this.hover]) b.ensure(plan.length);
    const c = this.center.data, sp = this.span.data, a = this.alpha.data, s = this.sel.data, h = this.hover.data;
    this.categories = new Array(plan.length);
    this.dataIndex = new Array(plan.length);
    for (let i = 0; i < plan.length; i++) {
      const p = plan[i];
      this.categories[i] = p.category;
      this.dataIndex[i] = p.dataIndex;
      c[2 * i] = p.c0; c[2 * i + 1] = p.c1;
      sp[4 * i] = p.v00; sp[4 * i + 1] = p.v01; sp[4 * i + 2] = p.v10; sp[4 * i + 3] = p.v11;
      a[2 * i] = p.a0; a[2 * i + 1] = p.a1;
      s[2 * i] = p.s0; s[2 * i + 1] = p.s1;
      h[i] = 0;
    }
    this.count = plan.length;
    this.hoverInstance = -1;
    this.geometry.instanceCount = plan.length;
    this.uDim.value = m.selected ? 1 : 0;
    for (const b of [this.center, this.span, this.alpha, this.sel, this.hover]) b.dirty();
    this.refreshColors(theme);
  }

  /** Highlight the bar for `category` (null clears). */
  setHover(category: string | null): boolean {
    const idx = category === null ? -1 : this.categories.indexOf(category);
    if (idx === this.hoverInstance) return false;
    const h = this.hover.data;
    if (this.hoverInstance >= 0) h[this.hoverInstance] = 0;
    if (idx >= 0) h[idx] = 1;
    this.hoverInstance = idx;
    this.hover.dirty();
    return true;
  }

  dispose(): void {
    this.geometry.dispose();
    this.material.dispose();
  }
}
