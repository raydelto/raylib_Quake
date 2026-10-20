// web_touch.mjs - play the browser build with the touch controls, and with
// keyboard and mouse (QUAK-31).
//
//   PLAYWRIGHT=/path/to/node_modules/playwright/index.mjs \
//     node scripts/web_touch.mjs [BUILD_DIR] [OUT_DIR] [--only pixel7|iphone14|desktop]
//       [--renderer gl]
//
// BUILD_DIR defaults to build-web, OUT_DIR to web-touch. Chromium only:
// Playwright's Pixel 7 and iPhone 14 emulation (landscape viewport, user
// agent, touch, coarse pointer) is not a real phone, so Android Chrome and
// iOS Safari themselves are not covered. Touches are real touch events
// (CDP Input.dispatchTouchEvent), several fingers at once.
//
// Phones, touch only: the overlay comes on by itself; title -> New Game
// (start map) -> E1M1 (typed into the console on the phone keyboard); in
// E1M1 save, then move with the stick while firing, look by dragging
// (yaw and pitch), jump, next weapon, save again, load the first save and
// check it came back; open and close the menu; turn to portrait and back
// (the game re-inits at the canvas size); no pointer lock is ever asked
// for, and the audio context runs. Game state
// is read from the save files (origin, view angles, shells, weapon).
// Desktop: no overlay, keyboard play, pointer lock on a click, the mouse
// looks and fires; ?touch=1 forces the overlay. Any page or console error
// fails the run. Page screenshots go to OUT_DIR. --renderer gl runs it all
// on the GLQuake build (?renderer=gl, QUAK-45).

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
const only = flag('--only', null);
const renderer = flag('--renderer', null);
const buildDir = resolve(args[0] || 'build-web');
const outDir = resolve(args[1] || 'web-touch');
mkdirSync(outDir, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 8900 + Math.floor(Math.random() * 300);
const server = spawn('python3', ['-m', 'http.server', String(port), '--bind', '127.0.0.1',
  '-d', buildDir], { stdio: 'ignore' });
const base = `http://127.0.0.1:${port}/index.html`;

const log = [];
const errors = [];
const result = { checks: {} };
let passed = 0, failed = 0;
function check(name, ok, detail) {
  result.checks[name] = { ok: !!ok, detail };
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
  if (ok)
    passed++;
  else
    failed++;
}

const browser = await pw.chromium.launch({ headless: true });
result.chromium = browser.version();

// Counts pointer lock requests, so a touch run can show it never asked.
const initScript = () => {
  window.__locks = 0;
  const orig = HTMLCanvasElement.prototype.requestPointerLock;
  HTMLCanvasElement.prototype.requestPointerLock = function () {
    window.__locks++;
    return orig.apply(this, arguments);
  };
};

class Page {
  constructor(page, name) {
    this.page = page;
    this.name = name;
    this.lines = [];
  }
  static async open(context, name, query) {
    const page = await context.newPage();
    const p = new Page(page, name);
    page.on('console', (m) => {
      const line = `[${name}] ${m.type()}: ${m.text()}`;
      log.push(line);
      p.lines.push(m.text());
      if (m.type() === 'error')
        errors.push(line);
    });
    page.on('pageerror', (e) => {
      log.push(`[${name}] pageerror: ${e.name}: ${e.message}`);
      errors.push(`[${name}] ${e.name}: ${e.message}`);
    });
    await page.addInitScript(initScript);
    for (let i = 0; i < 50; i++) {
      try { await fetch(base); break; } catch { await sleep(100); }
    }
    const q = [renderer ? `renderer=${renderer}` : '', query || ''].filter(Boolean).join('&');
    await page.goto(base + (q ? '?' + q : ''));
    await page.waitForSelector('#play:not([disabled])', { timeout: 60000 });
    return p;
  }
  eval(fn, arg) { return this.page.evaluate(fn, arg); }
  mark() { return this.lines.length; }
  // Waits for a console line matching re, printed after mark.
  async waitLine(re, mark, ms = 30000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      for (let i = mark; i < this.lines.length; i++)
        if (re.test(this.lines[i]))
          return true;
      await sleep(100);
    }
    return false;
  }
  async waitFor(fn, ms = 20000, arg) {
    try {
      await this.page.waitForFunction(fn, arg, { timeout: ms, polling: 100 });
      return true;
    } catch {
      return false;
    }
  }
  playing() { return this.eval(() => Module.quakePlaying); }
  shot(file) { return this.page.screenshot({ path: join(outDir, file) }); }
  // A file the game wrote, as text ('' if missing).
  readFile(path) {
    return this.eval((p) => {
      try { return new TextDecoder().decode(Module.FS.readFile(p)); } catch { return ''; }
    }, path);
  }
}

// Fingers through CDP: every event carries all the fingers still down.
class Fingers {
  constructor(cdp) {
    this.cdp = cdp;
    this.down = new Map();
    this.next = 1;
  }
  points() { return [...this.down.entries()].map(([id, p]) => ({ x: p.x, y: p.y, id })); }
  send(type) {
    return Promise.race([
      this.cdp.send('Input.dispatchTouchEvent', { type, touchPoints: this.points() }),
      sleep(10000).then(() => { throw new Error('page did not take a touch in 10 s (hung?)'); }),
    ]);
  }
  async start(x, y) {
    const id = this.next++;
    this.down.set(id, { x, y });
    await this.send('touchStart');
    return id;
  }
  async move(id, x, y) {
    this.down.set(id, { x, y });
    await this.send('touchMove');
  }
  async end(id) {
    this.down.delete(id);
    await this.send('touchEnd');
  }
  async drag(id, x0, y0, x1, y1, steps, ms) {
    for (let i = 1; i <= steps; i++) {
      await this.move(id, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
      await sleep(ms / steps);
    }
  }
  async tap(x, y, ms = 120) {
    const id = await this.start(x, y);
    await sleep(ms);
    await this.end(id);
  }
}

async function center(p, sel) {
  const b = await p.page.locator(sel).boundingBox();
  if (!b)
    throw new Error(`${sel} is not shown`);
  return { x: b.x + b.width / 2, y: b.y + b.height / 2 };
}

// The player in a save file: origin, view angles, shells, weapon.
function player(sav) {
  const m = sav.match(/\{[^{}]*"classname" "player"[^{}]*\}/);
  if (!m)
    return null;
  const field = (k) => {
    const f = m[0].match(new RegExp(`"${k}" "([^"]*)"`));
    return f ? f[1].split(' ').map(Number) : null;
  };
  return { origin: field('origin'), v_angle: field('v_angle') || [0, 0, 0],
           shells: (field('ammo_shells') || [0])[0], weapon: (field('weapon') || [0])[0] };
}
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const angleDiff = (a, b) => Math.abs(((a - b) % 360 + 540) % 360 - 180);

// A small greyscale copy of the game picture. The WebGL canvas cannot be
// read back once shown, so this decodes a screenshot in the page. A
// screenshot sometimes catches the cleared buffer between two frames, or a
// partly black one (headless Chromium, both seen again and again on the
// same spot): it is always darker than the real frame, so keep the
// brightest of three. A fixed "mean > 8" cutoff stopped working once the
// web build got brighter (QUAK-40): the partly black captures reach 9.
const mean = (sig) => sig.reduce((x, y) => x + y, 0) / sig.length;
async function canvasSig(p) {
  let best = null;
  for (let i = 0; i < 3; i++) {
    const sig = await canvasShot(p);
    if (!best || mean(sig) > mean(best))
      best = sig;
  }
  return best;
}
async function canvasShot(p) {
  const png = await p.page.locator('#canvas').screenshot();
  return p.eval(async (b64) => {
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const img = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const c = document.createElement('canvas');
    c.width = 160; c.height = 120;
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0, 160, 120);
    const d = g.getImageData(0, 0, 160, 120).data, out = [];
    for (let i = 0; i < d.length; i += 4)
      out.push((d[i] + d[i + 1] + d[i + 2]) / 3);
    return out;
  }, png.toString('base64'));
}
function diff(a, b) {
  let d = 0;
  for (let i = 0; i < a.length; i++)
    d += Math.abs(a[i] - b[i]);
  return d / a.length;
}

// After a resize: the canvas is drawn at its CSS size times devicePixelRatio,
// and the game prints a matching "Video mode WxH, canvas WxH" line (the
// frame is the canvas size, or less within vid_maxpixels).
async function fitsCanvas(p, mark, ms = 8000) {
  const want = () => p.eval(() => {
    const c = document.getElementById('canvas'), r = c.getBoundingClientRect();
    const d = devicePixelRatio || 1;
    return { w: Math.round(r.width * d), h: Math.round(r.height * d), cw: c.width, ch: c.height };
  });
  const end = Date.now() + ms;
  let w, line;
  while (Date.now() < end) {
    w = await want();
    line = null;
    for (let i = p.lines.length - 1; i >= mark && !line; i--) {
      const m = p.lines[i].match(/Video mode (\d+)x(\d+), (?:canvas|window) (\d+)x(\d+)/);
      if (m)
        line = m.slice(1).map(Number);
    }
    if (line && w.cw === w.w && w.ch === w.h && line[2] === w.w && line[3] === w.h
        && line[0] <= w.w && line[1] <= w.h)
      return { ok: true, detail: `canvas ${w.cw}x${w.ch}, frame ${line[0]}x${line[1]}` };
    await sleep(200);
  }
  return { ok: false, detail: `canvas ${w.cw}x${w.ch} (want ${w.w}x${w.h}), line ${line}` };
}

async function phone(key, device) {
  const tag = (s) => `${key}: ${s}`;
  const context = await browser.newContext({ ...pw.devices[device] });
  const p = await Page.open(context, key, 'console=1');
  const f = new Fingers(await context.newCDPSession(p.page));
  const vp = p.page.viewportSize();
  const T = (sel) => center(p, sel).then((c) => f.tap(c.x, c.y));
  const picture = () => f.tap(vp.width / 2, vp.height / 2);

  check(tag('coarse pointer turns touch on'), await p.eval(() =>
    matchMedia('(pointer: coarse)').matches && !document.querySelector('.touchhelp').classList.contains('hidden')));

  // The Play button by a tap: the game and its audio start from the gesture.
  await T('#play');
  check(tag('overlay shown after Play, menu layout during the demo'),
        await p.waitFor(() => !document.getElementById('touch').classList.contains('hidden')
                        && document.getElementById('touch').classList.contains('menu')
                        && document.body.classList.contains('touchplay')));
  await sleep(1500);
  await picture();           // the first touch also resumes audio
  await sleep(500);
  check(tag('audio context running'), await p.waitFor(() => window.miniaudio
        && miniaudio.devices.some((d) => d && d.webaudio && d.webaudio.state === 'running'), 5000));
  await p.shot(`${key}-title.png`);

  // Title -> main menu (that first tap) -> Single Player -> New Game
  let mk = p.mark();
  await picture();           // Single Player
  await sleep(400);
  await picture();           // New Game
  check(tag('New Game starts the start map by taps'), await p.waitLine(/Introduction/i, mk, 40000));
  check(tag('play layout while playing'), await p.waitFor(() => Module.quakePlaying === 1
        && document.getElementById('touch').classList.contains('play')));
  await sleep(1500);
  await p.shot(`${key}-start.png`);

  // E1M1: the console button (console=1), the phone keyboard, "map e1m1"
  await T('#tcon');
  check(tag('console gives the menu layout'), await p.waitFor(() => Module.quakePlaying === 0
        && document.getElementById('touch').classList.contains('menu')));
  await T('#tkbd');
  check(tag('keyboard button focuses the text input'),
        await p.waitFor(() => document.activeElement === document.getElementById('tinput'), 3000));
  mk = p.mark();
  await p.page.keyboard.type('map e1m1', { delay: 30 });
  await p.page.keyboard.press('Enter');
  check(tag('typed "map e1m1" loads E1M1'), await p.waitLine(/Slipgate Complex/i, mk, 40000));
  check(tag('playing again in E1M1'), await p.waitFor(() => Module.quakePlaying === 1));
  await sleep(2500);         // let the player land

  // Menu: MENU, then picture taps / arrow buttons, then a save slot.
  const save = async (spDowns, slotDowns, slotUps = 0) => {
    await T('#tmenu');
    await sleep(400);
    await picture();         // main menu: Single Player
    await sleep(300);
    for (let i = 0; i < spDowns; i++) { await T('#tdown'); await sleep(150); }
    for (let i = 0; i < -spDowns; i++) { await T('#tup'); await sleep(150); }
    await T('#tok');         // Save / Load
    await sleep(300);
    for (let i = 0; i < slotDowns; i++) { await T('#tdown'); await sleep(150); }
    for (let i = 0; i < slotUps; i++) { await T('#tup'); await sleep(150); }
    await picture();         // the slot
  };

  // Save s0: Single Player cursor 0 -> Save (2 down), slot 0.
  mk = p.mark();
  await save(2, 0);
  check(tag('save to s0 by touch'), await p.waitLine(/Saving game to .*s0\.sav/, mk, 15000));
  await p.waitFor(() => Module.quakePlaying === 1);
  const s0 = player(await p.readFile('/quake/id1/s0.sav'));
  check(tag('s0 has the player'), s0 && s0.origin, JSON.stringify(s0));

  // Menu open / close
  await T('#tmenu');
  const menuOpen = await p.waitFor(() => Module.quakePlaying === 0
                                   && document.getElementById('touch').classList.contains('menu'));
  await p.shot(`${key}-menu.png`);
  await T('#tback');
  const menuClosed = await p.waitFor(() => Module.quakePlaying === 1
                                     && document.getElementById('touch').classList.contains('play'));
  check(tag('MENU opens the menu, BACK closes it'), menuOpen && menuClosed);
  await sleep(300);

  // Jump, at the E1M1 start: once the player stands still, the picture
  // moves with no other input than JUMP. (Done before walking: the walk
  // can end under a low ledge, beside a strobing light, where nothing or
  // everything moves.)
  await sleep(2000);
  const a = await canvasSig(p);
  let still = 0;
  for (let i = 0; i < 2; i++) {
    await sleep(200);
    still = Math.max(still, diff(a, await canvasSig(p)));
  }
  // The arc lasts about 0.6 s and a screenshot of a large canvas can take
  // longer than that: press JUMP again and again (Quake jumps once per
  // press) for 2.4 s while sampling (single shots, blank and partly black
  // ones skipped), so samples land inside an arc.
  const jc = await center(p, '#tjump');
  let jumping = true;
  const presses = (async () => {
    while (jumping) {
      const jid = await f.start(jc.x, jc.y);
      await sleep(400);
      await f.end(jid);
      await sleep(100);
    }
  })();
  let jump = 0, samples = 0;
  for (const end = Date.now() + 2400; Date.now() < end;) {
    const sig = await canvasShot(p);
    if (mean(sig) > mean(a) / 2) {
      jump = Math.max(jump, diff(a, sig));
      samples++;
    }
  }
  jumping = false;
  await presses;
  check(tag('JUMP moves the view'), jump > 2 * still + 1,
        `still ${still.toFixed(2)}, jump ${jump.toFixed(2)} (${samples} samples)`);
  await sleep(800);

  // Move with the stick while firing (two fingers), then look.
  const fire = await center(p, '#tfire');
  const sx = vp.width * 0.2, sy = vp.height * 0.6;
  const stick = await f.start(sx, sy);
  await f.drag(stick, sx, sy, sx, sy - 80, 4, 80);       // full forward
  const fingerFire = await f.start(fire.x, fire.y);
  await sleep(1200);
  await p.shot(`${key}-play.png`);
  await f.end(fingerFire);
  await f.drag(stick, sx, sy - 80, sx + 80, sy, 4, 80);  // strafe right
  await sleep(500);
  await f.end(stick);
  const look = await f.start(vp.width * 0.6, vp.height * 0.5);
  await f.drag(look, vp.width * 0.6, vp.height * 0.5, vp.width * 0.6 + 120, vp.height * 0.5 - 40, 8, 300);
  await f.end(look);
  await sleep(400);

  // Next weapon
  await T('#tnext');
  await sleep(800);

  // Save s1: Single Player remembers Save; slot 1.
  mk = p.mark();
  await save(0, 1);
  check(tag('save to s1 by touch'), await p.waitLine(/Saving game to .*s1\.sav/, mk, 15000));
  await p.waitFor(() => Module.quakePlaying === 1);
  const s1 = player(await p.readFile('/quake/id1/s1.sav'));
  if (s0 && s1) {
    check(tag('stick moved the player'), dist(s0.origin, s1.origin) > 64,
          `${dist(s0.origin, s1.origin).toFixed(1)} units`);
    check(tag('drag turned the view (yaw)'), angleDiff(s0.v_angle[1], s1.v_angle[1]) > 20,
          `yaw ${s0.v_angle[1]} -> ${s1.v_angle[1]}`);
    check(tag('drag tilted the view (pitch)'), Math.abs(s1.v_angle[0] - s0.v_angle[0]) > 5,
          `pitch ${s0.v_angle[0]} -> ${s1.v_angle[0]}`);
    check(tag('FIRE used shells'), s1.shells < s0.shells, `shells ${s0.shells} -> ${s1.shells}`);
    check(tag('WPN changed the weapon'), s1.weapon !== s0.weapon, `weapon ${s0.weapon} -> ${s1.weapon}`);
  } else {
    check(tag('s1 has the player'), false);
  }

  // Load s0: Single Player is on Save, Load is one up; the slot list is on 1.
  mk = p.mark();
  await save(-1, 0, 1);
  check(tag('load s0 by touch'), await p.waitLine(/Loading game from .*s0\.sav/, mk, 15000));
  await p.waitFor(() => Module.quakePlaying === 1);
  await sleep(1000);
  // Save s2 to see what came back: Load -> Save is one down; slot 0 -> 2.
  mk = p.mark();
  await save(1, 2);
  await p.waitLine(/Saving game to .*s2\.sav/, mk, 15000);
  const s2 = player(await p.readFile('/quake/id1/s2.sav'));
  check(tag('loaded game is the s0 game'), s0 && s2 && dist(s0.origin, s2.origin) < 16
        && s2.shells === s0.shells && s2.weapon === s0.weapon,
        s2 && `origin off by ${dist(s0.origin, s2.origin).toFixed(1)}, shells ${s2.shells}, weapon ${s2.weapon}`);

  // Portrait: the game draws at the portrait canvas size; back to landscape.
  mk = p.mark();
  await p.page.setViewportSize({ width: vp.height, height: vp.width });
  const port = await fitsCanvas(p, mk);
  await sleep(500);
  await p.shot(`${key}-portrait.png`);
  mk = p.mark();
  await p.page.setViewportSize(vp);
  const land = await fitsCanvas(p, mk);
  check(tag('portrait and back re-init the video at the canvas size'), port.ok && land.ok,
        `${port.detail}; ${land.detail}`);
  check(tag('no rotate hint without ?rotate=1'), await p.eval(() =>
    document.getElementById('rotate').classList.contains('hidden')));

  check(tag('no pointer lock asked for'), await p.eval(() => window.__locks === 0
        && document.pointerLockElement === null));
  await context.close();
}

async function desktop() {
  const tag = (s) => `desktop: ${s}`;
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const p = await Page.open(context, 'desktop');
  check(tag('no touch help or overlay before play'), await p.eval(() =>
    document.querySelector('.touchhelp').classList.contains('hidden')
    && document.getElementById('touch').classList.contains('hidden')));
  await p.page.click('#play');
  await sleep(1500);
  check(tag('overlay stays hidden'), await p.eval(() =>
    document.getElementById('touch').classList.contains('hidden')
    && !document.body.classList.contains('touchplay')));
  const first = await fitsCanvas(p, 0);
  check(tag('frame is the canvas size at 1280x900'), first.ok, first.detail);
  let mk0 = p.mark();
  await p.page.setViewportSize({ width: 1000, height: 700 });
  const resized = await fitsCanvas(p, mk0);
  mk0 = p.mark();
  await p.page.setViewportSize({ width: 1280, height: 900 });
  const back = await fitsCanvas(p, mk0);
  check(tag('window resize re-inits at the canvas size'), resized.ok && back.ok,
        `${resized.detail}; ${back.detail}`);
  const kb = p.page.keyboard;
  const key = async (k, ms = 120) => { await kb.down(k); await sleep(ms); await kb.up(k); await sleep(250); };
  let mk = p.mark();
  await key('Escape');
  await key('Enter');        // Single Player
  await key('Enter');        // New Game
  check(tag('keyboard: Esc, Enter, Enter starts a game'), await p.waitLine(/Introduction/i, mk, 40000)
        && await p.waitFor(() => Module.quakePlaying === 1));
  await sleep(1500);
  const a = await canvasSig(p);
  await key('ArrowUp', 1000);
  const b = await canvasSig(p);
  check(tag('arrow keys move'), diff(a, b) > 1, `diff ${diff(a, b).toFixed(2)}`);
  await p.page.mouse.click(640, 400);
  check(tag('a click asks for pointer lock'), await p.waitFor(() => window.__locks >= 1, 3000));
  const locked = await p.waitFor(() => document.pointerLockElement === document.getElementById('canvas'), 3000);
  check(tag('pointer locked'), locked);
  if (locked) {
    const c = await canvasSig(p);
    // Some headless Chromium builds (151) report movementX/Y 0 for
    // Playwright's mouse moves under pointer lock, so send the moves the
    // way a locked mouse delivers them: mousemove events with movementX.
    for (let i = 0; i < 10; i++) {
      await p.eval(() => document.getElementById('canvas').dispatchEvent(
        new MouseEvent('mousemove', { bubbles: true, movementX: 15, movementY: 0 })));
      await sleep(30);
    }
    await sleep(300);
    const d = await canvasSig(p);
    check(tag('mouse looks'), diff(c, d) > 1, `diff ${diff(c, d).toFixed(2)}`);
  }
  await p.shot('desktop.png');
  await context.close();

  // ?touch=1 forces the overlay on a desktop.
  const ctx2 = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const q = await Page.open(ctx2, 'desktop-touch1', 'touch=1');
  await q.page.click('#play');
  check(tag('?touch=1 shows the overlay'), await q.waitFor(() =>
    !document.getElementById('touch').classList.contains('hidden')));
  await sleep(1000);
  await q.shot('desktop-touch1.png');
  await ctx2.close();
}

try {
  if (!only || only === 'pixel7')
    await phone('pixel7', 'Pixel 7 landscape');
  if (!only || only === 'iphone14')
    await phone('iphone14', 'iPhone 14 landscape');
  if (!only || only === 'desktop')
    await desktop();
} catch (e) {
  check('run finished', false, e.stack || String(e));
}
check('no page or console errors', errors.length === 0, errors.slice(0, 5).join(' | '));
result.passed = passed;
result.failed = failed;
writeFileSync(join(outDir, 'result.json'), JSON.stringify(result, null, 2));
writeFileSync(join(outDir, 'console.log'), log.join('\n') + '\n');
console.log(`${passed} passed, ${failed} failed (Chromium ${result.chromium})`);
await browser.close();
server.kill();
process.exit(failed ? 1 : 0);
