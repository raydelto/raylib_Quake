// web_renderers.mjs - the browser build's two renderers side by side
// (QUAK-45): the software renderer (the default page) and GLQuake on
// WebGL 2 (?renderer=gl), optionally next to https://www.netquake.io/quake.
//
//   PLAYWRIGHT=/path/to/node_modules/playwright/index.mjs \
//     node scripts/web_renderers.mjs [BUILD_DIR] [OUT_DIR]
//       [--gpu swiftshader|intel|nvidia] [--size 1920x1080] [--netquake]
//       [--no-fps] [--vsync]
//
// BUILD_DIR defaults to build-web, OUT_DIR to web-renderers. For each
// renderer, from a fresh profile, at --size (CSS pixels, device pixel ratio
// 1): `map start`, wait at the spawn, save the canvas (start-<r>.png) and
// the mean luma of the 3D view (as scripts/web_luma.mjs measures it); then
// `timedemo demo1` from the command line and the fps it prints. --netquake
// also captures netquake.io at the same spawn (typed into its console).
//
// --gpu picks Chromium's WebGL backend: swiftshader (the CPU, the default
// in headless Chromium), intel or nvidia (the real GPU through ANGLE on
// OpenGL; nvidia uses PRIME render offload). Chromium runs requestAnimation-
// Frame at the display's rate, and the browser build renders one frame per
// call, so a timedemo can't beat 60 fps; unless --vsync is given the
// browser is started with that limit off (--disable-gpu-vsync
// --disable-frame-rate-limit), so the timedemo measures the renderer.
// Results go to OUT_DIR/renderers-<gpu>.json.

import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';

const pw = await import(process.env.PLAYWRIGHT ||
  join(homedir(), '.npm/_npx/e41f203b7505f1fb/node_modules/playwright/index.mjs'));
const args = process.argv.slice(2);
const flag = (name, def) => {
  const i = args.indexOf(name);
  if (i < 0)
    return def;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const bool = (name) => {
  const i = args.indexOf(name);
  if (i < 0)
    return false;
  args.splice(i, 1);
  return true;
};
const gpu = flag('--gpu', 'swiftshader');
const [width, height] = flag('--size', '1920x1080').split('x').map(Number);
const netquake = bool('--netquake');
const noFps = bool('--no-fps');
const vsync = bool('--vsync');
const buildDir = resolve(args[0] || 'build-web');
const outDir = resolve(args[1] || 'web-renderers');
mkdirSync(outDir, { recursive: true });

const backends = {
  swiftshader: { args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] },
  intel: { args: ['--enable-gpu', '--ignore-gpu-blocklist', '--use-angle=gl'] },
  nvidia: {
    args: ['--enable-gpu', '--ignore-gpu-blocklist', '--use-angle=gl'],
    env: { __NV_PRIME_RENDER_OFFLOAD: '1', __GLX_VENDOR_LIBRARY_NAME: 'nvidia',
           __EGL_VENDOR_LIBRARY_FILENAMES: '/usr/share/glvnd/egl_vendor.d/10_nvidia.json' },
  },
};
if (!backends[gpu])
  throw new Error(`--gpu ${gpu}: one of ${Object.keys(backends).join(', ')}`);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 8900 + Math.floor(Math.random() * 300);
const server = spawn('python3', ['-m', 'http.server', String(port), '--bind', '127.0.0.1',
  '-d', buildDir], { stdio: 'ignore' });
const base = `http://127.0.0.1:${port}/index.html`;
for (let i = 0; i < 50; i++) {
  try { await fetch(base); break; } catch { await sleep(100); }
}

const browser = await pw.chromium.launch({
  headless: true,
  args: [...backends[gpu].args,
         ...(vsync ? [] : ['--disable-gpu-vsync', '--disable-frame-rate-limit'])],
  env: { ...process.env, ...(backends[gpu].env || {}) },
});
const errors = [];
const result = { chromium: browser.version(), gpu, size: `${width}x${height}`,
                 uncapped: !vsync, renderers: {} };

// Mean luma (Rec. 601) of a canvas PNG less its top 6% and bottom 12%.
async function luma(page, png) {
  return page.evaluate(async (b64) => {
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const img = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const c = document.createElement('canvas');
    c.width = img.width; c.height = img.height;
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0);
    const y0 = Math.round(img.height * 0.06), y1 = Math.round(img.height * 0.88);
    const d = g.getImageData(0, y0, img.width, y1 - y0).data;
    let s = 0;
    for (let i = 0; i < d.length; i += 4)
      s += 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
    return s / (d.length / 4);
  }, png.toString('base64'));
}

// A canvas capture that isn't a cleared (black) buffer.
async function capture(page, selector) {
  for (let i = 0; i < 6; i++) {
    const png = await page.locator(selector).screenshot();
    const y = await luma(page, png);
    if (y > 4)
      return { png, luma: y };
    await sleep(150);
  }
  throw new Error(`${selector}: only black frames`);
}

async function webglRenderer(page) {
  return page.evaluate(() => {
    const g = document.createElement('canvas').getContext('webgl2')
      || document.createElement('canvas').getContext('webgl');
    const e = g && g.getExtension('WEBGL_debug_renderer_info');
    return g ? g.getParameter(e ? e.UNMASKED_RENDERER_WEBGL : g.RENDERER) : 'none';
  });
}

async function open(key, query) {
  const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  const lines = [];
  page.on('console', (m) => {
    lines.push(m.text());
    if (m.type() === 'error')
      errors.push(`[${key}] ${m.text()}`);
  });
  page.on('pageerror', (e) => errors.push(`[${key}] ${e.name}: ${e.message}`));
  await page.goto(`${base}?${query}`);
  await page.waitForSelector('#play:not([disabled])', { timeout: 90000 });
  await page.click('#play');
  return { context, page, lines };
}

async function renderer(name, query) {
  const r = result.renderers[name] = {};
  const pre = query ? `${query}&` : '';

  const { context, page, lines } = await open(name, `${pre}args=${encodeURIComponent('+map start')}`);
  await page.waitForFunction(() => Module.quakePlaying === 1, null, { timeout: 60000 });
  await sleep(3000);
  await page.addStyleTag({ content: '#hint, #tools { visibility: hidden !important; }' });
  const ys = [];
  let shot;
  for (let i = 0; i < 3; i++) {
    shot = await capture(page, '#canvas');
    ys.push(shot.luma);
    await sleep(300);
  }
  writeFileSync(join(outDir, `start-${name}-${gpu}.png`), shot.png);
  r.luma = +(ys.reduce((a, b) => a + b, 0) / ys.length).toFixed(2);
  r.canvas = await page.evaluate(() => [canvas.width, canvas.height]);
  r.webgl = await webglRenderer(page);
  r.video = lines.filter((l) => /^(Video mode|GL_RENDERER|GL_VERSION)/.test(l));
  console.log(`${name}: start spawn luma ${r.luma}, canvas ${r.canvas.join('x')}, ${r.webgl}`);
  await context.close();

  if (noFps)
    return;
  const t = await open(`${name}-timedemo`, `${pre}args=${encodeURIComponent('+timedemo demo1')}`);
  const re = /(\d+) frames\s+([\d.]+) seconds\s+([\d.]+) fps/;
  for (let i = 0; i < 600 && !t.lines.some((l) => re.test(l)); i++)
    await sleep(500);
  const line = t.lines.find((l) => re.test(l));
  if (!line)
    throw new Error(`${name}: no timedemo result`);
  const m = line.match(re);
  r.timedemo = { frames: +m[1], seconds: +m[2], fps: +m[3] };
  console.log(`${name}: timedemo demo1 ${line.trim()}`);
  await t.context.close();
}

async function netquakeShot() {
  const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  await page.goto('https://www.netquake.io/quake', { waitUntil: 'load', timeout: 90000 });
  await sleep(8000);
  await page.keyboard.press('Backquote');
  await sleep(500);
  await page.keyboard.type('map start', { delay: 30 });
  await page.keyboard.press('Enter');
  await sleep(6000);
  const shot = await capture(page, 'canvas');
  writeFileSync(join(outDir, `start-netquake-${gpu}.png`), shot.png);
  result.renderers.netquake = { luma: +shot.luma.toFixed(2), webgl: await webglRenderer(page) };
  console.log(`netquake.io: start spawn luma ${shot.luma.toFixed(2)}`);
  await context.close();
}

try {
  await renderer('software', '');
  await renderer('gl', 'renderer=gl');
  if (netquake)
    await netquakeShot();
} finally {
  result.errors = errors;
  writeFileSync(join(outDir, `renderers-${gpu}.json`), JSON.stringify(result, null, 2));
  await browser.close();
  server.kill();
}
if (errors.length) {
  console.log(`page errors:\n${errors.join('\n')}`);
  process.exit(1);
}
