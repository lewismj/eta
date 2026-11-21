/**
 * Areas: filled regions between a lower and an upper edge (a line's fill to
 * the baseline, or a quantile band). 'gradient' fades toward the lower edge
 * so the fill reads as a soft glow under the curve; 'band' is flat.
 */

import * as THREE from 'three/webgpu';
import { Fn, attribute, float, mix, pow, uniform, varying, vec3 } from 'three/tsl';
import { CATEGORICAL } from '../color';
import { FrameUniforms, clipToRect, dataToPixel, flatMaterial } from '../gpu';
import type { AreaMark } from '../marks';
import { lerp } from '../scale';
import { areaTriangles, planArea } from '../geometry';

export class AreaLayer {
  readonly mesh: THREE.Mesh;
  private readonly geometry = new THREE.BufferGeometry();
  private readonly material = flatMaterial();
  private readonly uColor = uniform(new THREE.Color());
  private readonly uOpacity = uniform(0.3);
  private readonly uGradient = uniform(1);
  private slot = 0;
  /** Triangle list (x, y, v) at the start and end of the transition. */
  private trisFrom: Float32Array | null = null;
  private trisTo: Float32Array | null = null;

  constructor(u: FrameUniforms) {
    this.setAttributes(new Float32Array(0), new Float32Array(0), new Float32Array(0), new Float32Array(0));
    const t = u.uT;
    const pixel = dataToPixel(u, mix(attribute('aFrom', 'vec2'), attribute('aTo', 'vec2'), t));
    this.material.positionNode = vec3(pixel, 0);
    const vPixel = varying(pixel);
    const v = attribute('aV', 'float');
    const alphaPair = attribute('aAlpha', 'vec2');
    const fade = mix(alphaPair.x, alphaPair.y, t);
    // Gradient: strongest at the upper edge, fading to ~10% at the lower one.
    const ramp = mix(float(1), float(0.08).add(pow(v, 1.3).mul(0.92)), this.uGradient);
    this.material.colorNode = Fn(() => {
      clipToRect(u, vPixel);
      return this.uColor;
    })();
    this.material.opacityNode = this.uOpacity.mul(ramp).mul(fade);
    this.mesh = new THREE.Mesh(this.geometry, this.material);
    this.mesh.frustumCulled = false;
    this.mesh.renderOrder = 1;
  }

  private setAttributes(from: Float32Array, to: Float32Array, v: Float32Array, alpha: Float32Array): void {
    const n = v.length;
    const g = this.geometry;
    g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(3 * n), 3));
    g.setAttribute('aFrom', new THREE.BufferAttribute(from, 2));
    g.setAttribute('aTo', new THREE.BufferAttribute(to, 2));
    g.setAttribute('aV', new THREE.BufferAttribute(v, 1));
    g.setAttribute('aAlpha', new THREE.BufferAttribute(alpha, 2));
    g.setDrawRange(0, n);
  }

  refreshColors(theme: 'dark' | 'light'): void {
    this.uColor.value.set(CATEGORICAL[theme][this.slot]);
  }

  /** Triangle list at eased time t. */
  private trisAt(t: number): Float32Array | null {
    if (!this.trisTo) return null;
    if (!this.trisFrom || this.trisFrom.length !== this.trisTo.length) return this.trisTo;
    const out = new Float32Array(this.trisTo.length);
    for (let i = 0; i < out.length; i++) out[i] = lerp(this.trisFrom[i], this.trisTo[i], t);
    return out;
  }

  update(m: AreaMark, t: number, theme: 'dark' | 'light'): void {
    this.slot = m.slot;
    this.uOpacity.value = m.opacity;
    this.uGradient.value = m.fill === 'gradient' ? 1 : 0;
    const next = areaTriangles(m.xs, m.y0s, m.y1s, m.baseline);
    const now = this.trisAt(t);
    const plan = planArea(now, next);
    this.setAttributes(plan.from, plan.to, plan.v, plan.alpha);
    this.trisFrom = now && now.length === next.length ? now : next;
    this.trisTo = next;
    this.refreshColors(theme);
  }

  dispose(): void {
    this.geometry.dispose();
    this.material.dispose();
  }
}
