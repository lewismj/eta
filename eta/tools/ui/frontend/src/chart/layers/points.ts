/**
 * Scatter points: instanced anti-aliased dots with a 2px surface ring,
 * hover highlight and a luminous halo for the selected point. Positions,
 * opacity and selection interpolate on the GPU between keyed states.
 */

import * as THREE from 'three/webgpu';
import { Fn, attribute, float, length, max, mix, positionGeometry, smoothstep, uniform, uv, varying, vec3 } from 'three/tsl';
import { CATEGORICAL } from '../color';
import { FrameUniforms, InstanceBuffer, clipToRect, cssColor, dataToPixel, flatMaterial, quadGeometry } from '../gpu';
import type { PointsMark } from '../marks';
import { lerp } from '../scale';
import { InstanceNow, planTransition } from '../transition';

export class PointsLayer {
  readonly mesh: THREE.Mesh;
  private readonly geometry = quadGeometry();
  private readonly material = flatMaterial();
  private readonly from: InstanceBuffer;
  private readonly to: InstanceBuffer;
  private readonly alpha: InstanceBuffer;
  private readonly sel: InstanceBuffer;
  private readonly hover: InstanceBuffer;
  private slot = 0;
  count = 0;
  keys: string[] = [];
  dataIndex: number[] = [];
  private hoverInstance = -1;

  private readonly uSize = uniform(10);
  private readonly uColor = uniform(new THREE.Color());
  private readonly uSurface = uniform(new THREE.Color());

  constructor(u: FrameUniforms) {
    const g = this.geometry;
    this.from = new InstanceBuffer(g, 'aFrom', 2);
    this.to = new InstanceBuffer(g, 'aTo', 2);
    this.alpha = new InstanceBuffer(g, 'aAlpha', 2);
    this.sel = new InstanceBuffer(g, 'aSel', 2);
    this.hover = new InstanceBuffer(g, 'aHover', 1);

    const t = u.uT;
    const center = dataToPixel(u, mix(attribute('aFrom', 'vec2'), attribute('aTo', 'vec2'), t));
    const selPair = attribute('aSel', 'vec2');
    const sel = mix(selPair.x, selPair.y, t);
    const hov = attribute('aHover', 'float');
    const alphaPair = attribute('aAlpha', 'vec2');
    const fade = mix(alphaPair.x, alphaPair.y, t);

    const radius = this.uSize.mul(0.5).mul(float(1).add(sel.mul(0.4)).add(hov.mul(0.25)));
    const ringW = float(2);
    const halo = sel.mul(16).add(hov.mul(8));
    const extent = radius.add(ringW).add(halo).add(1.5);
    const pixel = center.add(positionGeometry.xy.mul(extent.mul(2)));
    this.material.positionNode = vec3(pixel, 0);
    const vPixel = varying(pixel);

    const dist = length(uv().sub(0.5)).mul(extent.mul(2));
    const aa = float(0.8);
    const core = float(1).sub(smoothstep(radius.sub(aa), radius.add(aa), dist));
    const outer = float(1).sub(smoothstep(radius.add(ringW).sub(aa), radius.add(ringW).add(aa), dist));
    const ring = max(outer.sub(core), 0);
    const glow = float(1)
      .sub(smoothstep(radius, radius.add(halo).add(0.001), dist))
      .mul(sel.mul(0.5).add(hov.mul(0.25)));

    const dotColor = mix(this.uColor, vec3(1, 1, 1), sel.mul(0.18));
    const inRing = ring.div(outer.add(0.0001));
    this.material.colorNode = Fn(() => {
      clipToRect(u, vPixel, 24);
      return mix(dotColor, this.uSurface, inRing);
    })();
    this.material.opacityNode = max(outer, glow).mul(fade);

    this.mesh = new THREE.Mesh(this.geometry, this.material);
    this.mesh.frustumCulled = false;
    this.mesh.renderOrder = 3;
  }

  refreshColors(theme: 'dark' | 'light'): void {
    this.uColor.value.set(CATEGORICAL[theme][this.slot]);
    this.uSurface.value.copy(cssColor('--surface-chart', theme === 'dark' ? '#1a1a19' : '#fcfcfb'));
  }

  stateAt(i: number, t: number) {
    const f = this.from.data, to = this.to.data, a = this.alpha.data, s = this.sel.data;
    return {
      x: lerp(f[2 * i], to[2 * i], t),
      y: lerp(f[2 * i + 1], to[2 * i + 1], t),
      alpha: lerp(a[2 * i], a[2 * i + 1], t),
      sel: lerp(s[2 * i], s[2 * i + 1], t),
      targetAlpha: a[2 * i + 1],
    };
  }

  update(m: PointsMark, t: number, theme: 'dark' | 'light'): void {
    this.slot = m.slot;
    this.uSize.value = Math.max(8, m.size ?? 10);
    const now: InstanceNow[] = [];
    for (let i = 0; i < this.count; i++) now.push({ key: this.keys[i], ...this.stateAt(i, t) });
    const plan = planTransition(now, m);

    for (const b of [this.from, this.to, this.alpha, this.sel, this.hover]) b.ensure(plan.length);
    this.keys = new Array(plan.length);
    this.dataIndex = new Array(plan.length);
    const f = this.from.data, to = this.to.data, a = this.alpha.data, s = this.sel.data, h = this.hover.data;
    for (let i = 0; i < plan.length; i++) {
      const e = plan[i];
      this.keys[i] = e.key;
      this.dataIndex[i] = e.dataIndex;
      f[2 * i] = e.fx; f[2 * i + 1] = e.fy;
      to[2 * i] = e.tx; to[2 * i + 1] = e.ty;
      a[2 * i] = e.a0; a[2 * i + 1] = e.a1;
      s[2 * i] = e.s0; s[2 * i + 1] = e.s1;
      h[i] = 0;
    }
    this.count = plan.length;
    this.hoverInstance = -1;
    this.geometry.instanceCount = this.count;
    for (const b of [this.from, this.to, this.alpha, this.sel, this.hover]) b.dirty();
    this.refreshColors(theme);
  }

  setHover(instance: number): boolean {
    if (instance === this.hoverInstance) return false;
    const h = this.hover.data;
    if (this.hoverInstance >= 0 && this.hoverInstance < this.count) h[this.hoverInstance] = 0;
    if (instance >= 0 && instance < this.count) h[instance] = 1;
    this.hoverInstance = instance;
    this.hover.dirty();
    return true;
  }

  dispose(): void {
    this.geometry.dispose();
    this.material.dispose();
  }
}
