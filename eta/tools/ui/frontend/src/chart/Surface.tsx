/**
 * Lit 3D surface (WebGPU with WebGL2 fallback).
 *
 * Grids are small, so animation runs on the CPU: heights and colours are
 * interpolated between keyed states and normals recomputed each frame,
 * which also keeps hover raycasting exact. The surface is upsampled with
 * Catmull-Rom splines; the original grid is drawn as faint lines on top.
 * Axis labels are HTML, projected from 3D every frame.
 */

import * as THREE from 'three/webgpu';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { For, Show, createEffect, createMemo, createSignal, onCleanup, onMount } from 'solid-js';
import type { UiNode } from '../model';
import { formatNumber } from '../format';
import { currentTheme, sequentialScale } from './color';
import { easeInOutCubic, ticks } from './scale';
import {
  Box, boxX, boxZ, displayResolution, nearestCell, parseSurface, resampleGrid, validateSurface, zRange,
} from './surfaceModel';

const TRANSITION_MS = 750;
const HEIGHT = 0.9;

interface Label {
  x: number;
  y: number;
  text: string;
  kind: 'tick' | 'title';
}

interface Hover {
  x: number;
  y: number;
  i: number;
  j: number;
}

export function Surface(props: { node: UiNode }) {
  let area!: HTMLDivElement;
  let canvas!: HTMLCanvasElement;
  const spec = createMemo(() => parseSurface(props.node.p as Record<string, unknown>));
  const problem = createMemo(() => validateSurface(spec()));
  const [labels, setLabels] = createSignal<Label[]>([]);
  const [hover, setHover] = createSignal<Hover | null>(null);
  const [failed, setFailed] = createSignal<string | null>(null);

  let renderer: THREE.WebGPURenderer | null = null;
  let controls: OrbitControls | null = null;
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(32, 1, 0.01, 50);
  const geometry = new THREE.PlaneGeometry(2, 2, 1, 1);
  const material = new THREE.MeshStandardNodeMaterial({
    vertexColors: true, roughness: 0.55, metalness: 0.05, side: THREE.DoubleSide,
  });
  const mesh = new THREE.Mesh(geometry, material);
  const gridLines = new THREE.LineSegments(
    new THREE.BufferGeometry(),
    new THREE.LineBasicNodeMaterial({ color: 0xffffff, transparent: true, opacity: 0.16, depthWrite: false }),
  );
  const frame3d = new THREE.LineSegments(
    new THREE.BufferGeometry(),
    new THREE.LineBasicNodeMaterial({ color: 0x898781, transparent: true, opacity: 0.45 }),
  );
  scene.add(mesh, gridLines, frame3d);
  scene.add(new THREE.HemisphereLight(0xdfe8ff, 0x1a1a19, 1.6));
  const sun = new THREE.DirectionalLight(0xffffff, 1.9);
  sun.position.set(1.5, 3, 2);
  scene.add(sun);

  // Animation state: displayed heights/colours interpolate from -> to.
  let ox = 2, oy = 2;
  let zFrom = new Float64Array(4), zTo = new Float64Array(4);
  let cFrom = new Float32Array(12), cTo = new Float32Array(12);
  let box: Box = { height: HEIGHT, zMin: 0, zMax: 1 };
  let boxFrom: Box = box;
  let start = -Infinity;
  let raf = 0;
  let ready = false;
  let size = { w: 1, h: 1 };

  const eased = () => easeInOutCubic((performance.now() - start) / TRANSITION_MS);

  const rebuildGeometry = (nx: number, ny: number) => {
    const g = new THREE.PlaneGeometry(2, 2, nx - 1, ny - 1);
    geometry.copy(g);
    g.dispose();
    geometry.setAttribute('color', new THREE.BufferAttribute(new Float32Array(nx * ny * 3), 3));
  };

  const currentHeights = (t: number): Float64Array => {
    const out = new Float64Array(zTo.length);
    const same = zFrom.length === zTo.length;
    for (let k = 0; k < zTo.length; k++) out[k] = same ? zFrom[k] + (zTo[k] - zFrom[k]) * t : zTo[k];
    return out;
  };

  const lerpBox = (t: number): Box => ({
    height: HEIGHT,
    zMin: boxFrom.zMin + (box.zMin - boxFrom.zMin) * t,
    zMax: boxFrom.zMax + (box.zMax - boxFrom.zMax) * t,
  });

  /** Write positions/colours for eased time t (rows along y, columns along x). */
  const apply = (t: number) => {
    const z = currentHeights(t);
    const b = lerpBox(t);
    const pos = geometry.getAttribute('position') as THREE.BufferAttribute;
    const col = geometry.getAttribute('color') as THREE.BufferAttribute;
    const same = cFrom.length === cTo.length;
    for (let j = 0; j < oy; j++) {
      for (let i = 0; i < ox; i++) {
        const k = j * ox + i;
        // Plane rows run top to bottom; map grid row j to box depth.
        const v = (oy - 1 - j) * ox + i;
        pos.setXYZ(v, boxX(i, ox), boxZ(z[k], b), -boxX(j, oy));
        for (let c = 0; c < 3; c++) {
          const a = same ? cFrom[3 * k + c] : cTo[3 * k + c];
          col.setComponent(v, c, a + (cTo[3 * k + c] - a) * t);
        }
      }
    }
    pos.needsUpdate = true;
    col.needsUpdate = true;
    geometry.computeVertexNormals();
    geometry.computeBoundingSphere();
    updateGridLines(z, b);
  };

  /** Faint lines along the original grid rows and columns. */
  const updateGridLines = (z: Float64Array, b: Box) => {
    const s = spec();
    const pts: number[] = [];
    const at = (i: number, j: number) => [boxX(i, ox), boxZ(z[j * ox + i], b) + 0.002, -boxX(j, oy)];
    const stepX = (ox - 1) / Math.max(1, s.nx - 1);
    const stepY = (oy - 1) / Math.max(1, s.ny - 1);
    for (let gj = 0; gj < s.ny; gj++) {
      const j = Math.round(gj * stepY);
      for (let i = 0; i + 1 < ox; i++) pts.push(...at(i, j), ...at(i + 1, j));
    }
    for (let gi = 0; gi < s.nx; gi++) {
      const i = Math.round(gi * stepX);
      for (let j = 0; j + 1 < oy; j++) pts.push(...at(i, j), ...at(i, j + 1));
    }
    gridLines.geometry.setAttribute('position', new THREE.Float32BufferAttribute(pts, 3));
  };

  const buildFrame = () => {
    const h = HEIGHT;
    const p = [
      -1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, -1, 1, 0, -1, -1, 0, -1, -1, 0, -1, -1, 0, 1, // floor
      -1, 0, -1, -1, h, -1, // vertical edge (z axis)
    ];
    frame3d.geometry.setAttribute('position', new THREE.Float32BufferAttribute(p, 3));
  };

  /** Retarget to the current spec. */
  const setData = () => {
    const s = spec();
    if (validateSurface(s)) return;
    const t = eased();
    const nowZ = currentHeights(t);
    const nowBox = lerpBox(t);
    const nx2 = displayResolution(s.nx), ny2 = displayResolution(s.ny);
    const z = resampleGrid(s.zs, s.nx, s.ny, nx2, ny2);
    const [lo, hi] = zRange(s.zs);
    const scale = sequentialScale(lo, hi, currentTheme());
    const colors = new Float32Array(nx2 * ny2 * 3);
    const c = new THREE.Color();
    for (let k = 0; k < z.length; k++) {
      const [r, g, b] = scale.color(z[k]);
      c.setRGB(r, g, b, THREE.SRGBColorSpace);
      colors[3 * k] = c.r;
      colors[3 * k + 1] = c.g;
      colors[3 * k + 2] = c.b;
    }
    const resized = nx2 !== ox || ny2 !== oy;
    if (resized) {
      ox = nx2;
      oy = ny2;
      rebuildGeometry(ox, oy);
    }
    const nowColors = resized ? colors : currentColors(t);
    zFrom = resized ? z : nowZ;
    zTo = z;
    cFrom = nowColors;
    cTo = colors;
    boxFrom = resized ? { height: HEIGHT, zMin: lo, zMax: hi } : nowBox;
    box = { height: HEIGHT, zMin: lo, zMax: hi };
    start = performance.now();
    requestFrame();
  };

  const currentColors = (t: number): Float32Array => {
    const out = new Float32Array(cTo.length);
    const same = cFrom.length === cTo.length;
    for (let k = 0; k < cTo.length; k++) out[k] = same ? cFrom[k] + (cTo[k] - cFrom[k]) * t : cTo[k];
    return out;
  };

  const project = (x: number, y: number, z: number) => {
    const v = new THREE.Vector3(x, y, z).project(camera);
    return { x: (v.x * 0.5 + 0.5) * size.w, y: (-v.y * 0.5 + 0.5) * size.h, visible: v.z < 1 };
  };

  const updateLabels = () => {
    const s = spec();
    if (validateSurface(s)) return;
    const out: Label[] = [];
    const b = lerpBox(eased());
    const xt = ticks(s.xs[0], s.xs[s.nx - 1], 5);
    for (const v of xt.values) {
      const bx = ((v - s.xs[0]) / (s.xs[s.nx - 1] - s.xs[0])) * 2 - 1;
      const p = project(bx, 0, 1.12);
      if (p.visible) out.push({ x: p.x, y: p.y, text: formatNumber(v, s.x.format ?? 'num', xt.step), kind: 'tick' });
    }
    const yt = ticks(s.ys[0], s.ys[s.ny - 1], 4);
    for (const v of yt.values) {
      const by = ((v - s.ys[0]) / (s.ys[s.ny - 1] - s.ys[0])) * 2 - 1;
      const p = project(1.12, 0, -by);
      if (p.visible) out.push({ x: p.x, y: p.y, text: formatNumber(v, s.y.format ?? 'num', yt.step), kind: 'tick' });
    }
    const zt = ticks(b.zMin, b.zMax, 4);
    for (const v of zt.values) {
      const p = project(-1.06, boxZ(v, b), -1.06);
      if (p.visible) out.push({ x: p.x, y: p.y, text: formatNumber(v, s.z.format ?? 'num', zt.step), kind: 'tick' });
    }
    const titles: [string | undefined, [number, number, number]][] = [
      [s.x.label, [0, 0, 1.32]],
      [s.y.label, [1.34, 0, 0]],
      [s.z.label, [-1.1, HEIGHT + 0.14, -1.1]],
    ];
    for (const [text, at] of titles) {
      if (!text) continue;
      const p = project(at[0], at[1], at[2]);
      if (p.visible) out.push({ x: p.x, y: p.y, text, kind: 'title' });
    }
    setLabels(out);
  };

  const requestFrame = () => {
    if (raf) return;
    raf = requestAnimationFrame(frame);
  };

  const frame = () => {
    raf = 0;
    if (!ready || !renderer) return;
    const t = eased();
    apply(t);
    const moving = controls?.update() ?? false;
    renderer.render(scene, camera);
    updateLabels();
    if (t < 1 || moving) requestFrame();
  };

  onMount(() => {
    try {
      renderer = new THREE.WebGPURenderer({ canvas, antialias: true, alpha: true });
    } catch (err) {
      setFailed(String(err));
      return;
    }
    renderer.setClearColor(0x000000, 0);
    camera.position.set(2.6, 1.9, 2.9);
    controls = new OrbitControls(camera, canvas);
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = 2;
    controls.maxDistance = 9;
    controls.maxPolarAngle = Math.PI * 0.49;
    controls.target.set(0, HEIGHT * 0.35, 0);
    controls.addEventListener('change', requestFrame);
    buildFrame();
    rebuildGeometry(ox, oy);

    renderer.init().then(() => {
      ready = true;
      requestFrame();
    }).catch((err) => setFailed(`GPU rendering unavailable: ${err}`));

    const ro = new ResizeObserver(() => {
      const r = area.getBoundingClientRect();
      size = { w: Math.max(1, r.width), h: Math.max(1, r.height) };
      renderer?.setPixelRatio(window.devicePixelRatio || 1);
      renderer?.setSize(size.w, size.h, false);
      camera.aspect = size.w / size.h;
      camera.updateProjectionMatrix();
      requestFrame();
    });
    ro.observe(area);

    onCleanup(() => {
      ro.disconnect();
      if (raf) cancelAnimationFrame(raf);
      controls?.dispose();
      geometry.dispose();
      material.dispose();
      gridLines.geometry.dispose();
      frame3d.geometry.dispose();
      renderer?.dispose();
      renderer = null;
    });
  });

  createEffect(() => {
    spec();
    setData();
    setHover(null);
  });

  const raycaster = new THREE.Raycaster();
  const onMove = (ev: PointerEvent) => {
    const r = area.getBoundingClientRect();
    const mx = ev.clientX - r.left, my = ev.clientY - r.top;
    raycaster.setFromCamera(new THREE.Vector2((mx / r.width) * 2 - 1, -(my / r.height) * 2 + 1), camera);
    const hit = raycaster.intersectObject(mesh)[0];
    if (!hit) {
      setHover(null);
      return;
    }
    const s = spec();
    const cell = nearestCell(hit.point.x, -hit.point.z, s.nx, s.ny);
    setHover({ x: mx, y: my, i: cell.i, j: cell.j });
  };

  const title = () => (typeof props.node.p.title === 'string' ? props.node.p.title : '');
  const subtitle = () => (typeof props.node.p.subtitle === 'string' ? props.node.p.subtitle : '');
  const height = () => (typeof props.node.p.height === 'number' ? `${props.node.p.height}px` : undefined);

  return (
    <section class="plot surface" style={{ height: height(), flex: height() ? 'none' : '1 1 0' }} data-testid="surface">
      <Show when={title() || subtitle()}>
        <header class="plot-head">
          <span class="plot-title">{title()}</span>
          <span class="plot-subtitle">{subtitle()}</span>
        </header>
      </Show>
      <div class="plot-area surface-area" ref={area} onPointerMove={onMove} onPointerLeave={() => setHover(null)}>
        <canvas ref={canvas} />
        <div class="plot-overlay">
          <For each={labels()}>
            {(l) => (
              <span class={l.kind === 'title' ? 'axis-title surface-title' : 'tick surface-tick'}
                style={{ left: `${l.x}px`, top: `${l.y}px` }}>
                {l.text}
              </span>
            )}
          </For>
        </div>
        <Show when={hover()}>
          {(h) => {
            const s = spec();
            return (
              <div class="tooltip" role="tooltip" style={{ left: `${h().x + 14}px`, top: `${h().y - 12}px` }}>
                <div class="tooltip-title">{formatNumber(s.zs[h().j * s.nx + h().i], s.z.format ?? 'num')}</div>
                <div class="tooltip-row"><span>{s.x.label ?? 'x'}</span><span class="num">{formatNumber(s.xs[h().i], s.x.format ?? 'num')}</span></div>
                <div class="tooltip-row"><span>{s.y.label ?? 'y'}</span><span class="num">{formatNumber(s.ys[h().j], s.y.format ?? 'num')}</span></div>
              </div>
            );
          }}
        </Show>
        <Show when={problem() || failed()}>
          <div class="plot-fallback" role="alert">{problem() ?? failed()}</div>
        </Show>
      </div>
    </section>
  );
}
