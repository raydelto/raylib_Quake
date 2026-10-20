// web_luma.mjs - how bright the browser build looks (QUAK-40).
//
//   PLAYWRIGHT=/path/to/node_modules/playwright/index.mjs \
//     node scripts/web_luma.mjs [BUILD_DIR] [OUT_DIR] [--args "+gamma 0.8"]
//       [--renderer gl]
//
// BUILD_DIR defaults to build-web, OUT_DIR to web-luma. For 1280x720,
// 1920x1080 and Playwright's Pixel 7 / iPhone 14 (landscape) emulation it
// starts `map start` from a fresh profile (no saved config.cfg), waits at
// the spawn and prints the mean luma (Rec. 601, 0-255) of the 3D view: the
// canvas less its top 6% (notify lines) and bottom 12% (status bar), the
// average of three captures. It also saves the page, the main menu over the
// game (Draw_FadeScreen) and, at 1920x1080, the E1M1 spawn. --args adds
// command-line options, e.g. to try a gamma value on an unchanged build;
// --renderer gl measures the GLQuake build (?renderer=gl).
// Results go to OUT_DIR/luma.json.

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
const extra = flag('--args', '');
const renderer = flag('--renderer', null);
const buildDir = resolve(args[0] || 'build-web');
const outDir = resolve(args[1] || 'web-luma');
mkdirSync(outDir, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 8900 + Math.floor(Math.random() * 300);
const server = spawn('python3', ['-m', 'http.server', String(port), '--bind', '127.0.0.1',
  '-d', buildDir], { stdio: 'ignore' });
const base = `http://127.0.0.1:${port}/index.html`;
for (let i = 0; i < 50; i++) {
  try { await fetch(base); break; } catch { await sleep(100); }
}

const browser = await pw.chromium.launch({ headless: true });
const errors = [];
const result = { chromium: browser.version(), args: extra, views: {} };

// Mean luma of the canvas, without the touch overlay and the portrait hint.
// A capture can catch the cleared (black) WebGL buffer: take another.
async function luma(page) {
  await page.addStyleTag({ content: '#touch, #rotate { visibility: hidden !important; }' });
  for (let i = 0; i < 6; i++) {
    const png = await page.locator('#canvas').screenshot();
    const y = await page.evaluate(async (b64) => {
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
    if (y > 4)
      return y;
    await sleep(100);
  }
  return 0;
}

async function view(key, contextOpts, extraShots) {
  const context = await browser.newContext(contextOpts);
  const page = await context.newPage();
  page.on('console', (m) => { if (m.type() === 'error') errors.push(`[${key}] ${m.text()}`); });
  page.on('pageerror', (e) => errors.push(`[${key}] ${e.name}: ${e.message}`));
  const q = encodeURIComponent(`${extra} +map start`.trim());
  await page.goto(`${base}?${renderer ? `renderer=${renderer}&` : ''}args=${q}`);
  await page.waitForSelector('#play:not([disabled])', { timeout: 60000 });
  await page.click('#play');
  await page.waitForFunction(() => Module.quakePlaying === 1, null, { timeout: 40000 });
  await sleep(3000);
  const ys = [];
  for (let i = 0; i < 3; i++) {
    ys.push(await luma(page));
    await sleep(300);
  }
  const mean = ys.reduce((a, b) => a + b, 0) / ys.length;
  await page.screenshot({ path: join(outDir, `${key}-start.png`) });
  await page.locator('#canvas').screenshot({ path: join(outDir, `${key}-start-canvas.png`) });
  await page.keyboard.press('Escape');
  await sleep(800);
  await page.locator('#canvas').screenshot({ path: join(outDir, `${key}-menu-canvas.png`) });
  result.views[key] = { luma: +mean.toFixed(2), samples: ys.map((v) => +v.toFixed(2)),
                        canvas: await page.evaluate(() => [canvas.width, canvas.height]) };
  console.log(`${key}: start-spawn view luma ${mean.toFixed(2)} (${ys.map((v) => v.toFixed(2)).join(', ')})`);
  if (extraShots)
    await extraShots(page);
  await context.close();
}

try {
  // Options -> Brightness all the way left (gamma 1), then all the way
  // right (gamma 0.5); the picture must get darker, then brighter.
  await view('1280x720', { viewport: { width: 1280, height: 720 } }, async (page) => {
    const kb = page.keyboard;
    // The menus remember their cursor, so only the first trip moves it.
    const slide = async (key, first) => {
      await kb.press('Escape');                 // main menu
      await sleep(300);
      const keys = first ? ['ArrowDown', 'ArrowDown', 'Enter', 'ArrowDown', 'ArrowDown', 'ArrowDown', 'ArrowDown']
                         : ['Enter'];
      for (const k of keys)
        { await kb.press(k); await sleep(120); }
      for (let i = 0; i < 12; i++)
        { await kb.press(key); await sleep(80); }
      await kb.press('Escape');
      await sleep(200);
      await kb.press('Escape');
      await sleep(1000);
      return +(await luma(page)).toFixed(2);
    };
    await kb.press('Escape');                   // close the menu opened above
    await sleep(500);
    const v = result.views['1280x720'];
    v.slider = { darkest: await slide('ArrowLeft', true), brightest: await slide('ArrowRight', false) };
    console.log(`1280x720: Brightness slider ends ${v.slider.darkest} .. ${v.slider.brightest}`);
  });
  await view('1920x1080', { viewport: { width: 1920, height: 1080 } }, async (page) => {
    await page.keyboard.press('Escape');
    await sleep(500);
    await page.keyboard.press('Backquote');
    await sleep(500);
    await page.keyboard.type('map e1m1');
    await page.keyboard.press('Enter');   // Host_Map_f closes the console
    await sleep(4000);
    result.views['1920x1080'].e1m1 = +(await luma(page)).toFixed(2);
    await page.locator('#canvas').screenshot({ path: join(outDir, '1920x1080-e1m1-canvas.png') });
  });
  await view('pixel7', { ...pw.devices['Pixel 7 landscape'] });
  await view('iphone14', { ...pw.devices['iPhone 14 landscape'] });
} finally {
  result.errors = errors;
  writeFileSync(join(outDir, 'luma.json'), JSON.stringify(result, null, 2));
  await browser.close();
  server.kill();
}
if (errors.length) {
  console.log(`page errors:\n${errors.join('\n')}`);
  process.exit(1);
}
