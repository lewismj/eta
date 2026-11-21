/**
 * GPU plot engine.
 *
 * three.js WebGPURenderer (falls back to WebGL2 automatically) with TSL node
 * materials, so shaders are written once for both backends.
 *
 * The scene is drawn in pixel space: an orthographic camera spans the canvas
 * in CSS pixels (y up), and every mark maps data to pixels in its vertex
 * shader using the current domain.
 *
 * Two independent eased clocks drive animation:
 *  - data clock (uT): positions, opacity and selection after new data;
 *  - domain clock (uDomT): axis rescaling, zoom and pan.
 * Zooming therefore never replays a data transition.
 */

import * as THREE from 'three/webgpu';
import { Theme, currentTheme } from './color';
import { FrameUniforms, makeFrameUniforms } from './gpu';
import type { Mark } from './marks';
import { Domain, easeInOutCubic, lerpDomain } from './scale';
import { PointsLayer } from './layers/points';
import { LineLayer } from './layers/line';
import { AreaLayer } from './layers/area';
import { BarsLayer } from './layers/bars';

export const TRANSITION_MS = 750;
export const ZOOM_MS = 320;

type AnyLayer = PointsLayer | LineLayer | AreaLayer | BarsLayer;

export interface PickResult {
  layerId: string;
  dataIndex: number;
  key: string;
  /** Pixel position of the point (y down, relative to the canvas). */
  px: number;
  py: number;
  instance: number;
}

export interface Margins {
  left: number;
  right: number;
  top: number;
  bottom: number;
}

function kindOf(layer: AnyLayer): Mark['kind'] {
  if (layer instanceof PointsLayer) return 'points';
  if (layer instanceof LineLayer) return 'line';
  if (layer instanceof AreaLayer) return 'area';
  return 'bars';
}

export class PlotEngine {
  readonly u: FrameUniforms = makeFrameUniforms();
  private readonly renderer: THREE.WebGPURenderer;
  private readonly scene = new THREE.Scene();
  private readonly camera = new THREE.OrthographicCamera(0, 1, 1, 0, -1, 1);
  private readonly layers = new Map<string, AnyLayer>();
  private domFrom: Domain = { x0: 0, x1: 1, y0: 0, y1: 1 };
  private domTo: Domain = { x0: 0, x1: 1, y0: 0, y1: 1 };
  private dataStart = -Infinity;
  private domStart = -Infinity;
  private domDuration = TRANSITION_MS;
  private raf = 0;
  private ready = false;
  private disposed = false;
  private theme: Theme = currentTheme();
  width = 1;
  height = 1;
  margins: Margins = { left: 60, right: 16, top: 10, bottom: 36 };

  /** Called every rendered frame with the current domain (for overlays). */
  onFrame: ((domain: Domain, settled: boolean) => void) | null = null;

  readonly init: Promise<void>;

  constructor(canvas: HTMLCanvasElement) {
    this.renderer = new THREE.WebGPURenderer({ canvas, antialias: true, alpha: true });
    this.renderer.setClearColor(0x000000, 0);
    this.init = this.renderer.init().then(() => {
      this.ready = true;
      this.requestFrame();
    });
  }

  get backendName(): string {
    const backend = (this.renderer as unknown as { backend?: { isWebGPUBackend?: boolean } }).backend;
    return backend?.isWebGPUBackend ? 'WebGPU' : 'WebGL2';
  }

  setMargins(m: Margins): void {
    this.margins = m;
    this.resize(this.width, this.height);
  }

  resize(width: number, height: number): void {
    this.width = Math.max(1, width);
    this.height = Math.max(1, height);
    this.renderer.setPixelRatio(window.devicePixelRatio || 1);
    this.renderer.setSize(this.width, this.height, false);
    this.camera.right = this.width;
    this.camera.top = this.height;
    this.camera.updateProjectionMatrix();
    const m = this.margins;
    this.u.uRect.value.set(m.left, m.bottom, Math.max(1, this.width - m.left - m.right),
      Math.max(1, this.height - m.top - m.bottom));
    this.requestFrame();
  }

  dataT(): number {
    return easeInOutCubic((performance.now() - this.dataStart) / TRANSITION_MS);
  }

  domainT(): number {
    return easeInOutCubic((performance.now() - this.domStart) / this.domDuration);
  }

  currentDomain(): Domain {
    return lerpDomain(this.domFrom, this.domTo, this.domainT());
  }

  targetDomain(): Domain {
    return this.domTo;
  }

  /**
   * Retarget every mark and animate to `domain`. Layers whose id is not in
   * `marks` are removed; a mark whose kind changed gets a fresh layer.
   */
  setMarks(marks: Mark[], domain: Domain): void {
    const t = this.dataT();
    const ids = new Set(marks.map((m) => m.id));
    for (const [id, layer] of this.layers) {
      const mark = marks.find((m) => m.id === id);
      if (!ids.has(id) || (mark && kindOf(layer) !== mark.kind)) this.removeLayer(id);
    }
    for (const m of marks) {
      let layer = this.layers.get(m.id);
      if (!layer) {
        layer = this.makeLayer(m.kind);
        this.layers.set(m.id, layer);
        this.scene.add(layer.mesh);
      }
      // Each layer's update narrows the mark to its own kind.
      (layer.update as (mark: Mark, t: number, theme: Theme) => void)(m, t, this.theme);
    }
    this.dataStart = performance.now();
    this.animateDomain(domain, TRANSITION_MS);
  }

  /** Animate the visible domain only (zoom / pan / reset). */
  setDomain(domain: Domain, durationMs = ZOOM_MS): void {
    this.animateDomain(domain, durationMs);
  }

  private animateDomain(domain: Domain, durationMs: number): void {
    this.domFrom = this.currentDomain();
    this.domTo = domain;
    this.domDuration = durationMs;
    this.domStart = performance.now();
    const f = this.domFrom;
    this.u.uDomFrom.value.set(f.x0, f.x1, f.y0, f.y1);
    this.u.uDomTo.value.set(domain.x0, domain.x1, domain.y0, domain.y1);
    this.requestFrame();
  }

  private makeLayer(kind: Mark['kind']): AnyLayer {
    switch (kind) {
      case 'points': return new PointsLayer(this.u);
      case 'line': return new LineLayer(this.u);
      case 'area': return new AreaLayer(this.u);
      case 'bars': return new BarsLayer(this.u);
    }
  }

  private removeLayer(id: string): void {
    const layer = this.layers.get(id);
    if (!layer) return;
    this.scene.remove(layer.mesh);
    layer.dispose();
    this.layers.delete(id);
  }

  setTheme(theme: Theme): void {
    this.theme = theme;
    for (const layer of this.layers.values()) layer.refreshColors(theme);
    this.requestFrame();
  }

  /** Map data coordinates to canvas pixels (y down) for domain `d`. */
  toPixels(x: number, y: number, d: Domain = this.currentDomain()): [number, number] {
    const m = this.margins;
    const w = this.width - m.left - m.right;
    const h = this.height - m.top - m.bottom;
    const px = m.left + ((x - d.x0) / (d.x1 - d.x0)) * w;
    const py = this.height - (m.bottom + ((y - d.y0) / (d.y1 - d.y0)) * h);
    return [px, py];
  }

  /** Inverse of toPixels. */
  fromPixels(px: number, py: number, d: Domain = this.currentDomain()): [number, number] {
    const m = this.margins;
    const w = this.width - m.left - m.right;
    const h = this.height - m.top - m.bottom;
    const x = d.x0 + ((px - m.left) / w) * (d.x1 - d.x0);
    const y = d.y0 + ((this.height - py - m.bottom) / h) * (d.y1 - d.y0);
    return [x, y];
  }

  /** Nearest visible scatter point within `radius` pixels of (mx, my). */
  pickPoint(mx: number, my: number, radius = 14): PickResult | null {
    const t = this.dataT();
    const d = this.currentDomain();
    let best: PickResult | null = null;
    let bestD2 = radius * radius;
    for (const [layerId, layer] of this.layers) {
      if (!(layer instanceof PointsLayer)) continue;
      for (let i = 0; i < layer.count; i++) {
        if (layer.dataIndex[i] < 0) continue;
        const s = layer.stateAt(i, t);
        const [px, py] = this.toPixels(s.x, s.y, d);
        const d2 = (px - mx) ** 2 + (py - my) ** 2;
        if (d2 <= bestD2) {
          bestD2 = d2;
          best = { layerId, dataIndex: layer.dataIndex[i], key: layer.keys[i], px, py, instance: i };
        }
      }
    }
    return best;
  }

  setPointHover(hit: PickResult | null): void {
    let changed = false;
    for (const [id, layer] of this.layers) {
      if (layer instanceof PointsLayer) changed = layer.setHover(hit && hit.layerId === id ? hit.instance : -1) || changed;
    }
    if (changed) this.requestFrame();
  }

  setBarHover(category: string | null): void {
    let changed = false;
    for (const layer of this.layers.values()) {
      if (layer instanceof BarsLayer) changed = layer.setHover(category) || changed;
    }
    if (changed) this.requestFrame();
  }

  requestFrame(): void {
    if (this.raf || this.disposed) return;
    this.raf = requestAnimationFrame(() => this.frame());
  }

  private frame(): void {
    this.raf = 0;
    if (!this.ready || this.disposed) return;
    const t = this.dataT();
    const dt = this.domainT();
    this.u.uT.value = t;
    this.u.uDomT.value = dt;
    this.renderer.render(this.scene, this.camera);
    const settled = t >= 1 && dt >= 1;
    this.onFrame?.(lerpDomain(this.domFrom, this.domTo, dt), settled);
    if (!settled) this.requestFrame();
  }

  dispose(): void {
    this.disposed = true;
    if (this.raf) cancelAnimationFrame(this.raf);
    for (const id of [...this.layers.keys()]) this.removeLayer(id);
    this.renderer.dispose();
  }
}
