'use strict';
// Loose Balloons: the heap's object graph as a night sky to fly through. The page is sent the graph
// but not what is alive. Every shot is put to the GC, which decides.
const $ = id => document.getElementById(id);
const canvas = $('sky');
const c = canvas.getContext('2d');
const TAU = Math.PI * 2;
const LAMP = 430;
const calm = matchMedia('(prefers-reduced-motion: reduce)').matches;
const PALETTE = ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#0ca30c', '#9085e9', '#e66767'];
const OTHER = '#77788c';
const GOLD = '#ffce6b';
const REFUSED = '#ff8d6b';
const ROPE = '#c9c4e8';

const TOWER_ROOM = 190;
const CHAIN_COST = 50;
const CHAIN_LENGTH = 8;
const MAX_HULL = 3;
const MAX_CROWS = 5;
const SECONDS_PER_ROPE = 0.28;

const game = {
  mode: 'wait', // or practice, round, over
  nodes: [],
  edges: [],
  world: 1200,
  plane: null,
  bullets: [],
  crows: [],
  bits: [],
  rings: [],
  points: 0,
  combo: 1,
  comboUntil: 0,
  chain: 0,
  flares: 2,
  hull: MAX_HULL,
  repair: 0,
  lives: 3,
  maxLives: 3,
  deadline: null,
  freedBytes: 0,
  freedCount: 0,
  wave: null,
  trail: null,
  shake: 0,
  chainRun: null,
  nextCrow: 0,
  time: 0,
};
let apps = [];

function bytes(n) {
  if (n < 1024) return `${n} B`;
  for (const unit of ['KB', 'MB', 'GB']) {
    n /= 1024;
    if (n < 1024 || unit === 'GB') return `${n.toFixed(n < 10 ? 1 : 0)} ${unit}`;
  }
}

const clamp = (value, low, high) => Math.max(low, Math.min(high, value));

// How far to turn to get from one heading to another, between -PI and PI.
const turnBetween = (from, to) => Math.atan2(Math.sin(to - from), Math.cos(to - from));

// A seeded random number generator (mulberry32), so that a heap is laid out the same on every page.
function randomFrom(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const rootNames = ['a stack', 'a handle', 'a pinned handle', 'a ref-counted handle'];
const colours = new Map();

function load(nodes, edges, seed, gone = []) {
  const random = randomFrom(seed);
  game.nodes = nodes.map((n, id) => ({
    id,
    type: n.type,
    count: n.count,
    bytes: n.bytes,
    root: n.rootKind,
    out: [],
    in: [],
    radius: Math.min(40, 11 + Math.log2(1 + n.bytes / 32) * 2.3),
    sway: random() * TAU,
    x: 0,
    y: 0,
    gone: gone.includes(id),
    hit: 0,
    shield: 0,
    parent: -1,
    depth: -1,
  }));
  game.edges = edges.map(([from, to]) => ({ from: game.nodes[from], to: game.nodes[to], phase: random() }));
  for (const e of game.edges) {
    e.from.out.push(e.to);
    e.to.in.push(e.from);
  }

  const byType = new Map();
  for (const n of game.nodes) byType.set(n.type, (byType.get(n.type) ?? 0) + n.bytes);
  for (const [type] of [...byType].sort((a, b) => b[1] - a[1]))
    if (colours.size < PALETTE.length && !colours.has(type)) colours.set(type, PALETTE[colours.size]);
  for (const n of game.nodes) n.colour = colours.get(n.type) ?? OTHER;
  layout(random);
}

// Lays the graph out as trees: one around the tower, and one for every island of loose balloons.
// Islands are put among the live branches and laid out the same way, so that a balloon's place in the
// sky does not give away whether it is garbage.
function layout(random) {
  const placed = new Set();
  // Adds everything the seeds lead to that is not in a tree yet, breadth first.
  const grow = seeds => {
    const order = [...seeds];
    for (const n of seeds) placed.add(n);
    for (let i = 0; i < order.length; i++) {
      for (const next of order[i].out) {
        if (placed.has(next)) continue;
        placed.add(next);
        next.parent = order[i].id;
        next.depth = order[i].depth + 1;
        order.push(next);
      }
    }
    return order;
  };
  const roots = game.nodes.filter(n => n.root >= 0);
  for (const n of roots) n.depth = 1;
  const live = grow(roots);

  // Spreads a tree over the angles between from and to. Larger subtrees get a wider share.
  const branch = (order, centreX, centreY, reach, from, to) => {
    const weight = new Map();
    for (const n of [...order].reverse()) {
      weight.set(n, (weight.get(n) ?? 0) + 1);
      const p = game.nodes[n.parent];
      if (p && order.includes(p)) weight.set(p, (weight.get(p) ?? 0) + weight.get(n));
    }
    const span = new Map();
    const fan = (children, start, end) => {
      const total = children.reduce((s, n) => s + Math.sqrt(weight.get(n)), 0) || 1;
      let at = start;
      for (const n of children) {
        const width = ((end - start) * Math.sqrt(weight.get(n))) / total;
        span.set(n, [at, at + width]);
        at += width;
      }
    };
    const tops = order.filter(n => n.parent < 0 || !order.includes(game.nodes[n.parent]));
    fan(tops, from, to);
    for (const n of order) {
      const [start, end] = span.get(n);
      const angle = (start + end) / 2;
      const depth = n.depth - tops[0].depth;
      const distance = reach * (tops.length > 1 ? 1 + depth * 0.75 : depth * 0.85) * (0.8 + random() * 0.4);
      n.x = centreX + Math.cos(angle) * distance;
      n.y = centreY + Math.sin(angle) * distance;
      fan(
        order.filter(m => m.parent === n.id),
        start - 0.15,
        end + 0.15,
      );
    }
  };
  branch(live, 0, 0, 290, 0, TAU);

  // Islands start from whatever the fewest others hold.
  const islands = [];
  const free = game.nodes.filter(n => !placed.has(n)).sort((a, b) => a.in.length - b.in.length);
  for (const n of free) {
    if (placed.has(n)) continue;
    n.depth = 0;
    islands.push(grow([n]));
  }
  const maxDepth = Math.max(2, ...live.map(n => Math.min(n.depth, 5)));
  for (const island of islands) {
    const friend = island.flatMap(n => n.out).find(n => !island.includes(n) && placed.has(n) && (n.x || n.y));
    const angle = random() * TAU;
    const distance = 300 + random() * 190 * maxDepth;
    const x = friend ? friend.x + Math.cos(angle) * 190 : Math.cos(angle) * distance;
    const y = friend ? friend.y + Math.sin(angle) * 190 : Math.sin(angle) * distance;
    const heading = random() * TAU;
    branch(island, x, y, 150, heading, heading + 4.5);
  }

  separate();
  game.world = 300 + Math.max(700, ...game.nodes.map(n => Math.hypot(n.x, n.y)));
  for (const n of game.nodes) {
    n.homeX = n.x;
    n.homeY = n.y;
  }
}

function separate() {
  for (let pass = 0; pass < 90; pass++) {
    for (let i = 0; i < game.nodes.length; i++) {
      const a = game.nodes[i];
      for (let j = i + 1; j < game.nodes.length; j++) {
        const b = game.nodes[j];
        const dx = b.x - a.x;
        const dy = b.y - a.y;
        const gap = a.radius + b.radius + 40;
        const d2 = dx * dx + dy * dy;
        if (d2 >= gap * gap) continue;
        const d = Math.sqrt(d2) || 1;
        const push = ((gap - d) / d) * 0.4;
        a.x -= dx * push + 0.01;
        a.y -= dy * push;
        b.x += dx * push;
        b.y += dy * push + 0.01;
      }
      const d = Math.hypot(a.x, a.y) || 1;
      if (d < TOWER_ROOM) {
        a.x *= TOWER_ROOM / d;
        a.y *= TOWER_ROOM / d;
      }
    }
  }
}

// Marks from the tower along the ropes. Flares, the explanation of a refused shot, crows and practice
// flights use this; in a real round the GC has the last word.
function reachable() {
  const parent = new Map();
  const order = [];
  for (const n of game.nodes) {
    if (n.root < 0 || n.gone) continue;
    parent.set(n, null);
    order.push(n);
  }
  for (let i = 0; i < order.length; i++) {
    for (const next of order[i].out) {
      if (next.gone || parent.has(next)) continue;
      parent.set(next, order[i]);
      order.push(next);
    }
  }
  return { parent, order };
}

function practiceGraph(seed) {
  const random = randomFrom(seed);
  const pick = n => Math.floor(random() * n);
  const types = ['Hangar', 'Plane', 'Pilot', 'string', 'Beacon', 'byte[]', 'Radar', 'Freight'];
  const nodes = [];
  const edges = [];
  const add = root => {
    nodes.push({
      type: types[pick(types.length)],
      count: 1 + (random() < 0.2 ? pick(9) : 0),
      bytes: 24 * 2 ** pick(9),
      rootKind: root,
    });
    return nodes.length - 1;
  };
  const tree = (root, size) => {
    const mine = [add(root)];
    while (mine.length < size) {
      const n = add(-1);
      edges.push([mine[pick(Math.min(mine.length, (6 + mine.length) >> 1))], n]);
      mine.push(n);
    }
    for (let i = 0; i < size / 3; i++) {
      const a = mine[pick(size)];
      const b = mine[pick(size)];
      if (a !== b) edges.push([a, b]);
    }
    return mine;
  };
  const live = Array.from({ length: 6 }, (_, i) => tree(i % 3, 5 + pick(6))).flat();
  for (let i = 0; i < 9; i++) {
    const island = tree(-1, 2 + pick(7));
    if (random() < 0.6) edges.push([island[pick(island.length)], live[pick(live.length)]]);
    if (random() < 0.5) edges.push([island[island.length - 1], island[0]]);
  }
  return { nodes, edges };
}

function resetFlight() {
  Object.assign(game, {
    bullets: [],
    crows: [],
    bits: [],
    rings: [],
    points: 0,
    combo: 1,
    comboUntil: 0,
    chain: 0,
    flares: 2,
    hull: MAX_HULL,
    wave: null,
    trail: null,
    shake: 0,
    chainRun: null,
    nextCrow: game.time + 12,
    freedBytes: 0,
    freedCount: 0,
  });
  game.plane = {
    x: 0,
    y: 120,
    heading: Math.PI / 2,
    speed: 240,
    reload: 0,
    safeUntil: game.time + 2,
    bank: 0,
  };
  hud();
}

function practice() {
  const { nodes, edges } = practiceGraph(Date.now() & 0xffff);
  load(nodes, edges, 7);
  game.mode = 'practice';
  game.lives = game.maxLives = 3;
  game.deadline = null;
  resetFlight();
  $('panel').hidden = true;
  toast('Practice: no app is frozen. Enter ends the flight.');
}

function showPanel(title, text, button) {
  $('panelTitle').textContent = title;
  $('panelText').textContent = text;
  $('go').textContent = button;
  $('panel').hidden = false;
  showApps();
}

function land() {
  if (game.mode === 'round') {
    send({ type: 'done' });
  } else if (game.mode === 'practice') {
    const { parent } = reachable();
    const missed = game.nodes.filter(n => !n.gone && !parent.has(n));
    endFlight(
      'Practice over.',
      missed.reduce((sum, n) => sum + n.bytes, 0),
    );
  }
}

function endFlight(why, missed) {
  const points = `${game.points.toLocaleString()} points.`;
  const objects = `${game.freedCount.toLocaleString()} object${game.freedCount === 1 ? '' : 's'}`;
  const freed = `${bytes(game.freedBytes)} freed from ${objects}.`;
  const left = missed > 0 ? `${bytes(missed)} of garbage got away.` : 'Not a byte of garbage got away.';
  game.mode = 'over';
  game.wave = null;
  showPanel(why, `${points} ${freed}\n${left}`, 'Practice flight');
  hud();
}

let toastTimer;
function toast(text, bad = false) {
  $('toast').textContent = text;
  $('toast').classList.toggle('bad', bad);
  $('toast').hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(
    () => {
      $('toast').hidden = true;
    },
    bad ? 5000 : 3000,
  );
}

function hud() {
  $('lives').textContent = '♥'.repeat(game.lives) + '♡'.repeat(Math.max(0, game.maxLives - game.lives));
  $('lives').setAttribute('aria-label', `${game.lives} of ${game.maxLives} lives`);
  $('hull').textContent = `hull ${'▮'.repeat(game.hull)}${'▯'.repeat(MAX_HULL - game.hull)}`;
  $('points').textContent = game.points.toLocaleString();
  $('combo').textContent = game.combo > 1 ? `×${game.combo}` : '';
  $('freed').textContent = `${bytes(game.freedBytes)} freed`;
  $('flares').textContent = '✺'.repeat(game.flares);
  $('chain').style.transform = `scaleX(${game.chain / 100})`;
  $('chainBox').classList.toggle('ready', game.chain >= CHAIN_COST);
  $('land').disabled = game.mode !== 'round' && game.mode !== 'practice';
  $('land').textContent = game.mode === 'practice' ? 'End practice' : 'Land: resume the app';
}

const flying = () => (game.mode === 'round' || game.mode === 'practice') && game.plane;

// Takes one answer about one balloon. Freeing a large group can take several.
function onHit(node, live, freedBytes, freedCount) {
  if (live) {
    node.hit = 0;
    node.shield = game.time + 4;
    game.combo = 1;
    game.shake = calm ? 0 : 14;
    showWhyAlive(node);
    sound('alive');
    game.chainRun = null;
  } else if (freedCount > 0) {
    game.freedBytes += freedBytes;
    game.freedCount += freedCount;
    node.left = (node.left ?? node.count) - freedCount;
    if (node.left <= 0 && !node.gone) pop(node);
  }
  hud();
}

function burst(from, count, make) {
  for (let i = 0; i < count; i++) {
    const angle = Math.random() * TAU;
    const { speed, life, colour, size } = make();
    const vx = Math.cos(angle) * speed;
    const vy = Math.sin(angle) * speed;
    game.bits.push({ x: from.x, y: from.y, vx, vy, life, colour, size });
  }
}

function pop(node) {
  node.gone = true;
  const gained = Math.round(Math.sqrt(node.bytes) * 10 * game.combo);
  game.points += gained;
  game.combo = Math.min(8, game.combo + 1);
  game.comboUntil = game.time + 5;
  game.chain = Math.min(100, game.chain + 9);
  burst(node, 26, () => ({
    speed: 40 + Math.random() * 220,
    life: 0.5 + Math.random() * 0.7,
    colour: node.colour,
    size: 2 + Math.random() * 4,
  }));
  game.rings.push({ x: node.x, y: node.y, r: node.radius, life: 0.5, colour: node.colour });
  game.bits.push({
    x: node.x,
    y: node.y - node.radius,
    vx: 0,
    vy: -40,
    life: 1.2,
    text: `+${gained.toLocaleString()}`,
    colour: GOLD,
  });
  if (!calm) game.shake = Math.max(game.shake, 4);
  sound('pop', node.radius);
  if (game.chainRun?.waiting === node) nextLink();
}

function showWhyAlive(node) {
  const { parent } = reachable();
  if (!parent.has(node)) return;
  const path = [];
  for (let n = node; n; n = parent.get(n)) path.unshift(n);
  game.trail = { path, until: game.time + 5 };
  const names = path.map(n => n.type);
  const shown = names.length > 6 ? [...names.slice(0, 3), '…', ...names.slice(-2)] : names;
  toast(`Alive, and a life lost. Held by ${rootNames[path[0].root]} → ${shown.join(' → ')}`, true);
}

function shoot(node) {
  if (node.gone || node.hit || node.shield > game.time) return false;
  node.hit = game.time;
  if (game.mode === 'round') {
    send({ type: 'shoot', node: node.id });
  } else {
    const live = reachable().parent.has(node);
    if (live) game.lives--;
    onHit(node, live, node.bytes, live ? 0 : node.count);
    if (game.lives <= 0) endFlight('Out of lives.', 1);
  }
  return true;
}

function fire() {
  const p = game.plane;
  if (p.reload > 0 || game.lives <= 0) return;
  p.reload = 0.17;
  for (const side of [-7, 7])
    bullet(p.x - Math.sin(p.heading) * side, p.y + Math.cos(p.heading) * side, p.heading, false);
  sound('shot');
}

function bullet(x, y, heading, chain) {
  game.bullets.push({
    x: x + Math.cos(heading) * 22,
    y: y + Math.sin(heading) * 22,
    vx: Math.cos(heading) * 820,
    vy: Math.sin(heading) * 820,
    life: 0.8,
    chain,
  });
}

function fireChain() {
  if (!flying() || game.chain < CHAIN_COST || game.chainRun || game.lives <= 0) return;
  game.chain -= CHAIN_COST;
  bullet(game.plane.x, game.plane.y, game.plane.heading, true);
  sound('chain');
  hud();
}

// Pops a balloon and then what it holds, one at a time. Ends early at anything alive (see onHit).
function startChain(node) {
  const links = [node];
  const seen = new Set(links);
  for (let i = 0; i < links.length && links.length < CHAIN_LENGTH; i++) {
    for (const next of links[i].out) {
      if (next.gone || seen.has(next) || links.length >= CHAIN_LENGTH) continue;
      seen.add(next);
      links.push(next);
    }
  }
  game.chainRun = { links, at: 0, waiting: null };
  nextLink();
}

function nextLink() {
  const run = game.chainRun;
  if (!run) return;
  while (run.at < run.links.length) {
    const node = run.links[run.at++];
    if (node.gone) continue;
    const from = run.waiting ?? game.plane;
    run.waiting = node;
    game.rings.push({ x: node.x, y: node.y, r: node.radius * 2.2, life: 0.35, colour: '#9085e9' });
    game.bits.push({ bolt: [from.x, from.y, node.x, node.y], x: 0, y: 0, vx: 0, vy: 0, life: 0.35 });
    setTimeout(() => {
      if (game.chainRun === run && flying() && !shoot(node)) game.chainRun = null;
    }, 130);
    return;
  }
  game.chainRun = null;
}

function flare() {
  if (!flying() || game.flares <= 0 || game.wave) return;
  game.flares--;
  game.combo = 1;
  const { parent, order } = reachable();
  // When the light gets to each balloon. It takes the same time for every rope on the way.
  const arrive = new Map();
  for (const n of order) {
    const holder = parent.get(n);
    arrive.set(n, (holder ? arrive.get(holder) : 0) + SECONDS_PER_ROPE);
  }
  game.wave = { start: game.time, arrive, end: Math.max(0, ...arrive.values()) + 3 };
  toast('Marking. Whatever stays dark is garbage.');
  sound('flare');
  hud();
}

function crash() {
  game.hull--;
  game.plane.safeUntil = game.time + 1.5;
  if (!calm) game.shake = 12;
  sound('hurt');
  if (game.hull <= 0) {
    burst(game.plane, 40, () => ({ speed: Math.random() * 300, life: 1, colour: '#f3ecd8', size: 3 }));
    toast('Wrecked by crows. A life lost.', true);
    game.hull = MAX_HULL;
    Object.assign(game.plane, { x: 0, y: 120, heading: Math.PI / 2, safeUntil: game.time + 3 });
    if (game.mode === 'round') send({ type: 'crash' });
    else if (--game.lives <= 0) endFlight('Out of lives.', 1);
  }
  hud();
}

const keys = new Set();
let mouse = null;
let mouseSteers = false;
let firing = false;
const flightKeys = ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'KeyA', 'KeyD', 'KeyW', 'Space'];

addEventListener('keydown', e => {
  if (e.target instanceof HTMLButtonElement || e.target instanceof HTMLAnchorElement) {
    if (e.code === 'Space' || e.code === 'Enter') return;
  }
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if (flightKeys.includes(e.code)) {
    keys.add(e.code);
    e.preventDefault();
    if (e.code !== 'Space' && e.code !== 'ArrowUp' && e.code !== 'KeyW') mouseSteers = false;
  }
  if (e.repeat) return;
  if (e.code === 'KeyF') flare();
  if (e.code === 'KeyX') fireChain();
  if (e.code === 'KeyM') toggleSound();
  if (e.code === 'Enter' && flying()) land();
});
addEventListener('keyup', e => keys.delete(e.code));
addEventListener('blur', () => {
  keys.clear();
  firing = false;
});
canvas.addEventListener('pointermove', e => {
  mouse = { x: e.clientX, y: e.clientY };
  mouseSteers = true;
});
canvas.addEventListener('pointerdown', e => {
  if (e.button === 2) {
    fireChain();
    return;
  }
  mouse = { x: e.clientX, y: e.clientY };
  mouseSteers = true;
  firing = true;
  canvas.setPointerCapture(e.pointerId);
});
for (const name of ['pointerup', 'pointercancel'])
  canvas.addEventListener(name, () => {
    firing = false;
  });
canvas.addEventListener('contextmenu', e => e.preventDefault());
$('land').onclick = () => {
  $('land').blur();
  land();
};
$('go').onclick = () => {
  $('go').blur();
  practice();
};
$('sound').onclick = () => {
  $('sound').blur();
  toggleSound();
};

let audio = null;
let soundOn = true;
function toggleSound() {
  soundOn = !soundOn;
  $('sound').setAttribute('aria-pressed', soundOn);
  $('sound').style.opacity = soundOn ? 1 : 0.4;
}
function sound(kind, size = 20) {
  if (!soundOn || !navigator.userActivation?.hasBeenActive) return;
  audio ??= new AudioContext();
  // Wave shape, start and end frequency, length in seconds, volume.
  const recipes = {
    shot: [['square', 620, 240, 0.07, 0.03]],
    pop: [
      ['triangle', 900 - size * 14, 160, 0.22, 0.16],
      ['sine', 1300 - size * 10, 500, 0.12, 0.08],
    ],
    alive: [['sawtooth', 170, 70, 0.5, 0.16]],
    hurt: [['sawtooth', 240, 60, 0.3, 0.14]],
    chain: [['square', 300, 1200, 0.3, 0.06]],
    flare: [
      ['sine', 260, 1040, 1.2, 0.1],
      ['sine', 390, 1560, 1.2, 0.06],
    ],
  };
  for (const [type, from, to, length, volume] of recipes[kind]) {
    const o = audio.createOscillator();
    const g = audio.createGain();
    const t = audio.currentTime;
    o.type = type;
    o.frequency.setValueAtTime(from, t);
    o.frequency.exponentialRampToValueAtTime(to, t + length);
    g.gain.setValueAtTime(volume, t);
    g.gain.exponentialRampToValueAtTime(0.0001, t + length);
    o.connect(g).connect(audio.destination);
    o.start(t);
    o.stop(t + length);
  }
}

function step(dt) {
  game.time += dt;
  drift(dt);
  if (!flying()) return;
  steer(dt);
  moveBullets(dt);
  if (!flying()) return;
  moveCrows(dt);
}

// Everything that goes on moving when nobody is flying.
function drift(dt) {
  for (const n of game.nodes) {
    n.x = n.homeX + Math.sin(game.time * 0.5 + n.sway) * 7;
    n.y = n.homeY + Math.cos(game.time * 0.4 + n.sway * 1.7) * 9;
    // If the app went away, the answer never comes. Let the balloon be shot again.
    if (n.hit && game.time - n.hit > 6) n.hit = 0;
  }
  for (const b of game.bits) {
    b.life -= dt;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    b.vx *= 1 - 1.5 * dt;
    b.vy *= 1 - 1.5 * dt;
  }
  game.bits = game.bits.filter(b => b.life > 0);
  for (const r of game.rings) {
    r.life -= dt;
    r.r += 160 * dt;
  }
  game.rings = game.rings.filter(r => r.life > 0);
  game.shake = Math.max(0, game.shake - 30 * dt);
  if (game.wave && game.time - game.wave.start > game.wave.end) game.wave = null;
  if (game.trail && game.time > game.trail.until) game.trail = null;
}

function steer(dt) {
  const p = game.plane;
  let turn = 0;
  if (keys.has('ArrowLeft') || keys.has('KeyA')) turn = -1;
  if (keys.has('ArrowRight') || keys.has('KeyD')) turn = 1;
  if (mouseSteers && mouse && turn === 0) {
    const want = Math.atan2(mouse.y - innerHeight / 2, mouse.x - innerWidth / 2);
    turn = clamp(turnBetween(p.heading, want) * 3, -1, 1);
  }
  const fromTower = Math.hypot(p.x, p.y);
  if (fromTower > game.world) {
    const home = Math.atan2(-p.y, -p.x);
    turn = clamp(turnBetween(p.heading, home) * 2, -1, 1);
  }
  const throttle = keys.has('ArrowUp') || keys.has('KeyW');
  p.speed += ((throttle ? 430 : 230) - p.speed) * Math.min(1, dt * 3);
  p.heading += turn * 3.4 * dt;
  p.bank += (turn - p.bank) * Math.min(1, dt * 8);
  p.x += Math.cos(p.heading) * p.speed * dt;
  p.y += Math.sin(p.heading) * p.speed * dt;
  p.reload -= dt;
  if (throttle || Math.random() < 0.4) {
    game.bits.push({
      x: p.x - Math.cos(p.heading) * 16,
      y: p.y - Math.sin(p.heading) * 16,
      vx: 0,
      vy: 0,
      life: 0.5,
      colour: '#ffffff55',
      size: 2,
    });
  }
  if (keys.has('Space') || firing) fire();
  if (game.combo > 1 && game.time > game.comboUntil) {
    game.combo = 1;
    hud();
  }
  if (fromTower < 170 && game.hull < MAX_HULL) {
    game.repair += dt;
    if (game.repair > 1.5) {
      game.hull++;
      game.repair = 0;
      toast('Repaired.');
      hud();
    }
  }
}

function moveBullets(dt) {
  for (const b of game.bullets) {
    b.life -= dt;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    const crow = game.crows.find(k => Math.hypot(k.x - b.x, k.y - b.y) < 20);
    if (crow) {
      b.life = 0;
      if (--crow.health <= 0) {
        crow.gone = true;
        game.points += 50;
        feathers(crow);
        hud();
      }
      continue;
    }
    const node = game.nodes.find(n => !n.gone && (n.x - b.x) ** 2 + (n.y - b.y) ** 2 < (n.radius + 4) ** 2);
    if (node) {
      b.life = 0;
      if (b.chain) startChain(node);
      else shoot(node);
      if (!flying()) return;
    }
  }
  game.bullets = game.bullets.filter(b => b.life > 0);
}

function moveCrows(dt) {
  const p = game.plane;
  if (game.time > game.nextCrow) {
    game.nextCrow = game.time + 7 + Math.random() * 5;
    const { parent } = reachable();
    const roosts = game.nodes.filter(n => !n.gone && !parent.has(n) && Math.hypot(n.x, n.y) > 250);
    if (roosts.length > 0 && game.crows.length < MAX_CROWS) {
      const from = roosts[Math.floor(Math.random() * roosts.length)];
      game.crows.push({ x: from.x, y: from.y, heading: 0, health: 2, flap: Math.random() * TAU });
    }
  }
  for (const k of game.crows) {
    const want = Math.atan2(p.y - k.y, p.x - k.x);
    k.heading += clamp(turnBetween(k.heading, want), -1.6 * dt, 1.6 * dt);
    k.x += Math.cos(k.heading) * 190 * dt;
    k.y += Math.sin(k.heading) * 190 * dt;
    k.flap += dt * 14;
    if (Math.hypot(k.x - p.x, k.y - p.y) < 24 && game.time > p.safeUntil) {
      k.gone = true;
      feathers(k);
      crash();
      if (!flying()) return;
    }
  }
  game.crows = game.crows.filter(k => !k.gone);
}

function feathers(crow) {
  burst(crow, 12, () => ({ speed: 120, life: 0.8, colour: '#3a3d55', size: 3 }));
}

const stars = Array.from({ length: 260 }, () => ({
  x: Math.random(),
  y: Math.random(),
  z: 0.05 + Math.random() * 0.3,
  s: Math.random() * 1.6 + 0.3,
}));
const clouds = Array.from({ length: 14 }, () => ({
  x: Math.random() * 3000,
  y: Math.random() * 3000,
  r: 120 + Math.random() * 200,
  z: 0.5 + Math.random() * 0.3,
}));
let width = 0;
let height = 0;

function resize() {
  const ratio = devicePixelRatio || 1;
  width = innerWidth;
  height = innerHeight;
  canvas.width = width * ratio;
  canvas.height = height * ratio;
  c.setTransform(ratio, 0, 0, ratio, 0, 0);
}
addEventListener('resize', resize);
resize();

const wrap = (v, size) => ((v % size) + size) % size;

function circle(x, y, radius) {
  c.beginPath();
  c.arc(x, y, radius, 0, TAU);
}

// A path through points given as x, y, x, y and so on.
function tracePath(...points) {
  c.beginPath();
  c.moveTo(points[0], points[1]);
  for (let i = 2; i < points.length; i += 2) c.lineTo(points[i], points[i + 1]);
}

function draw() {
  const p = game.plane ?? { x: 0, y: 0, heading: 0, bank: 0 };
  drawSky(p);
  c.save();
  const shake = game.shake;
  c.translate(
    width / 2 - p.x + (Math.random() - 0.5) * shake,
    height / 2 - p.y + (Math.random() - 0.5) * shake,
  );
  drawWorld(p);
  c.restore();
  if (flying()) {
    drawCompass(p);
    drawMap(p);
  }
  nameTarget(p);
}

function drawSky(p) {
  const sky = c.createLinearGradient(0, 0, 0, height);
  sky.addColorStop(0, '#090b1f');
  sky.addColorStop(0.65, '#191c45');
  sky.addColorStop(1, '#3a2a55');
  c.fillStyle = sky;
  c.fillRect(0, 0, width, height);
  c.fillStyle = '#fff';
  for (const s of stars) {
    c.globalAlpha = 0.35 + 0.4 * Math.sin(game.time * 1.5 + s.x * 90) ** 2;
    c.fillRect(wrap(s.x * width - p.x * s.z, width), wrap(s.y * height - p.y * s.z, height), s.s, s.s);
  }
  c.globalAlpha = 1;
  for (const k of clouds) {
    const x = wrap(k.x - p.x * k.z, width + 800) - 400;
    const y = wrap(k.y - p.y * k.z, height + 800) - 400;
    const g = c.createRadialGradient(x, y, 0, x, y, k.r);
    g.addColorStop(0, '#8a8fd012');
    g.addColorStop(1, '#8a8fd000');
    c.fillStyle = g;
    c.fillRect(x - k.r, y - k.r, k.r * 2, k.r * 2);
  }
}

function drawWorld(p) {
  const view = {
    left: p.x - width / 2 - 80,
    right: p.x + width / 2 + 80,
    top: p.y - height / 2 - 80,
    bottom: p.y + height / 2 + 80,
  };
  const seen = o => o.x > view.left && o.x < view.right && o.y > view.top && o.y < view.bottom;

  if (game.plane && flying()) {
    const lamp = c.createRadialGradient(p.x, p.y, 20, p.x, p.y, LAMP);
    lamp.addColorStop(0, '#ffe9b020');
    lamp.addColorStop(1, '#ffe9b000');
    c.fillStyle = lamp;
    circle(p.x, p.y, LAMP);
    c.fill();
  }
  drawTower();
  drawRopes(p, seen);
  for (const n of game.nodes) if (!n.gone && seen(n)) drawBalloon(n, p);
  for (const k of game.crows) drawCrow(k);
  for (const b of game.bullets) {
    c.strokeStyle = b.chain ? '#b9aaff' : '#ffe9a8';
    c.lineWidth = b.chain ? 5 : 2.5;
    tracePath(b.x, b.y, b.x - b.vx * 0.018, b.y - b.vy * 0.018);
    c.stroke();
  }
  for (const r of game.rings) {
    c.globalAlpha = Math.max(0, r.life * 1.6);
    c.strokeStyle = r.colour;
    c.lineWidth = 3;
    circle(r.x, r.y, r.r);
    c.stroke();
  }
  for (const b of game.bits) {
    c.globalAlpha = Math.min(1, b.life * 2);
    if (b.bolt) {
      drawBolt(...b.bolt);
    } else if (b.text) {
      c.fillStyle = b.colour;
      c.font = '700 16px ui-rounded, system-ui';
      c.textAlign = 'center';
      c.fillText(b.text, b.x, b.y);
    } else {
      c.fillStyle = b.colour;
      c.fillRect(b.x - b.size / 2, b.y - b.size / 2, b.size, b.size);
    }
  }
  c.globalAlpha = 1;
  if (flying()) drawPlane(p);
}

function drawTower() {
  c.save();
  const beam = game.time * 0.7;
  for (const side of [0, Math.PI]) {
    const g = c.createRadialGradient(0, -70, 0, 0, -70, 420);
    g.addColorStop(0, '#ffce6b55');
    g.addColorStop(1, '#ffce6b00');
    c.fillStyle = g;
    c.beginPath();
    c.moveTo(0, -70);
    c.arc(0, -70, 420, beam + side - 0.16, beam + side + 0.16);
    c.fill();
  }
  c.fillStyle = '#262a52';
  c.strokeStyle = '#5b609c';
  c.lineWidth = 2;
  tracePath(-26, 60, -13, -58, 13, -58, 26, 60);
  c.closePath();
  c.fill();
  c.stroke();
  for (let y = 40; y > -50; y -= 30) {
    tracePath(-22 + (60 - y) * 0.07, y, 18 - (60 - y) * 0.05, y - 24);
    c.stroke();
  }
  c.fillStyle = GOLD;
  c.shadowColor = GOLD;
  c.shadowBlur = 24;
  circle(0, -70, 12);
  c.fill();
  c.shadowBlur = 0;
  c.fillStyle = '#b9b7c9';
  c.font = '600 12px ui-rounded, system-ui';
  c.textAlign = 'center';
  c.fillText('ROOT', 0, 78);
  c.restore();
}

// How brightly the plane's lamp lights a rope, by the distance to its nearest point.
function ropeLight(a, b, p) {
  if (game.mode === 'over') return 0.5;
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const t = clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy || 1), 0, 1);
  const d = Math.hypot(a.x + dx * t - p.x, a.y + dy * t - p.y);
  return Math.max(0, 1 - d / LAMP) * 0.9;
}

function drawRopes(p, seen) {
  const tower = { x: 0, y: -70 };
  const wave = game.wave;
  const now = wave ? game.time - wave.start : 0;
  const trail = new Set();
  if (game.trail) {
    const names = ['root', ...game.trail.path.map(n => n.id)];
    for (let i = 1; i < names.length; i++) trail.add(`${names[i - 1]}>${names[i]}`);
  }
  const rope = (a, b, phase, key, marked) => {
    if (!seen(a) && !seen(b)) return;
    let light = ropeLight(a, b, p);
    let colour = ROPE;
    if (marked !== undefined && now > marked - SECONDS_PER_ROPE) {
      light = 1;
      colour = GOLD;
    }
    if (trail.has(key)) {
      light = 1;
      colour = REFUSED;
    }
    if (light < 0.03) return;
    const mx = (a.x + b.x) / 2;
    const my = (a.y + b.y) / 2 + Math.min(46, Math.hypot(b.x - a.x, b.y - a.y) * 0.12);
    c.globalAlpha = light;
    c.strokeStyle = colour;
    c.lineWidth = trail.has(key) ? 3 : 1.4;
    c.beginPath();
    c.moveTo(a.x, a.y);
    c.quadraticCurveTo(mx, my, b.x, b.y);
    c.stroke();
    c.fillStyle = colour === ROPE ? '#fff3c9' : colour;
    for (let i = 0; i < 2; i++) {
      const t = (game.time * 0.45 + phase + i / 2) % 1;
      const u = 1 - t;
      const x = u * u * a.x + 2 * u * t * mx + t * t * b.x;
      const y = u * u * a.y + 2 * u * t * my + t * t * b.y;
      circle(x, y, 2.6);
      c.fill();
    }
    // An arrowhead shows the same in a screenshot.
    const angle = Math.atan2(b.y - my, b.x - mx);
    const r = (b.radius ?? 0) + 5;
    c.save();
    c.translate(b.x - Math.cos(angle) * r, b.y - Math.sin(angle) * r);
    c.rotate(angle);
    tracePath(0, 0, -9, -4.5, -9, 4.5);
    c.fill();
    c.restore();
  };
  for (const n of game.nodes)
    if (n.root >= 0 && !n.gone) rope(tower, n, n.sway / TAU, `root>${n.id}`, wave?.arrive.get(n));
  for (const e of game.edges) {
    if (e.from.gone || e.to.gone) continue;
    const marked =
      wave?.arrive.has(e.from) && wave.arrive.has(e.to)
        ? wave.arrive.get(e.from) + SECONDS_PER_ROPE
        : undefined;
    rope(e.from, e.to, e.phase, `${e.from.id}>${e.to.id}`, marked);
  }
  c.globalAlpha = 1;
}

function drawBalloon(n, p) {
  const wave = game.wave;
  const marked = wave?.arrive.has(n) && game.time - wave.start > wave.arrive.get(n);
  const near = Math.max(0, 1 - Math.hypot(n.x - p.x, n.y - p.y) / (LAMP * 1.5));
  const r = n.radius;
  const wobble = n.hit ? Math.sin(game.time * 50) * 3 : 0;
  c.save();
  c.translate(n.x + wobble, n.y);
  if (game.mode === 'over') c.globalAlpha = 0.9;
  else if (wave) c.globalAlpha = marked ? 1 : 0.3;
  else c.globalAlpha = 0.5 + 0.5 * near;
  if (marked) {
    c.shadowColor = GOLD;
    c.shadowBlur = 30;
  }
  const g = c.createRadialGradient(-r * 0.35, -r * 0.4, r * 0.1, 0, 0, r * 1.05);
  g.addColorStop(0, '#ffffffcc');
  g.addColorStop(0.25, n.colour);
  g.addColorStop(1, '#0b0d22');
  c.fillStyle = g;
  circle(0, 0, r);
  c.fill();
  c.shadowBlur = 0;
  c.fillStyle = n.colour;
  tracePath(-4, r + 5, 0, r - 1, 4, r + 5);
  c.fill();
  if (n.count > 1) {
    c.fillStyle = '#fff';
    c.font = `700 ${clamp(r * 0.55, 10, 15)}px ui-rounded, system-ui`;
    c.textAlign = 'center';
    c.textBaseline = 'middle';
    c.fillText(`×${n.count >= 10000 ? `${Math.round(n.count / 1000)}k` : n.count}`, 0, 1);
  }
  if (n.shield > game.time) {
    c.globalAlpha = Math.min(1, n.shield - game.time);
    c.strokeStyle = REFUSED;
    c.lineWidth = 3;
    circle(0, 0, r + 8);
    c.stroke();
  }
  c.restore();
}

function drawPlane(p) {
  c.save();
  c.translate(p.x, p.y);
  c.rotate(p.heading);
  c.globalAlpha = game.time < p.safeUntil && Math.sin(game.time * 20) > 0 ? 0.35 : 1;
  const w = 13 * (1 - Math.abs(p.bank) * 0.35);
  c.fillStyle = '#f3ecd8';
  tracePath(22, 0, -14, -w, -7, 0);
  c.closePath();
  c.fill();
  c.fillStyle = '#cfc6ab';
  tracePath(22, 0, -14, w, -7, 0);
  c.closePath();
  c.fill();
  c.strokeStyle = '#8d8467';
  c.lineWidth = 1;
  tracePath(22, 0, -7, 0);
  c.stroke();
  c.restore();
}

function drawCrow(k) {
  c.save();
  c.translate(k.x, k.y);
  c.rotate(k.heading);
  c.scale(1, Math.cos(k.heading) < 0 ? -1 : 1);
  const flap = Math.sin(k.flap) * 9;
  c.fillStyle = '#05060f';
  c.strokeStyle = '#ff6b6b';
  c.lineWidth = 1.2;
  c.beginPath();
  c.moveTo(16, 0);
  c.quadraticCurveTo(0, -6, -14, 2);
  c.quadraticCurveTo(0, 8, 16, 0);
  c.fill();
  c.stroke();
  c.beginPath();
  c.moveTo(4, 0);
  c.lineTo(-8, -10 - flap);
  c.lineTo(-9, 1);
  c.moveTo(4, 0);
  c.lineTo(-8, 10 + flap);
  c.lineTo(-9, 1);
  c.fill();
  c.stroke();
  c.fillStyle = '#ff6b6b';
  circle(10, -1.5, 1.8);
  c.fill();
  c.restore();
}

function drawBolt(x1, y1, x2, y2) {
  c.strokeStyle = '#c8bcff';
  c.lineWidth = 3;
  c.beginPath();
  c.moveTo(x1, y1);
  for (let i = 1; i < 7; i++) {
    const x = x1 + ((x2 - x1) * i) / 7 + (Math.random() - 0.5) * 26;
    const y = y1 + ((y2 - y1) * i) / 7 + (Math.random() - 0.5) * 26;
    c.lineTo(x, y);
  }
  c.lineTo(x2, y2);
  c.stroke();
}

function drawCompass(p) {
  if (Math.abs(p.x) < width / 2 && Math.abs(p.y) < height / 2) return;
  const angle = Math.atan2(-p.y, -p.x);
  const r = Math.min(width, height) / 2 - 60;
  c.save();
  c.translate(width / 2 + Math.cos(angle) * r, height / 2 + Math.sin(angle) * r);
  c.rotate(angle);
  c.fillStyle = GOLD;
  tracePath(14, 0, -8, -8, -8, 8);
  c.fill();
  c.restore();
}

function drawMap(p) {
  const size = 150;
  const x = width - size - 16;
  const y = height - size - 16;
  const k = size / 2 / game.world;
  c.save();
  c.globalAlpha = 0.85;
  c.fillStyle = '#0b0d22';
  c.strokeStyle = '#ffffff30';
  c.lineWidth = 1;
  circle(x + size / 2, y + size / 2, size / 2);
  c.fill();
  c.stroke();
  c.clip();
  c.translate(x + size / 2, y + size / 2);
  c.fillStyle = '#9a9cc0';
  for (const n of game.nodes) if (!n.gone) c.fillRect(n.x * k - 1, n.y * k - 1, 2, 2);
  c.fillStyle = '#ff6b6b';
  for (const b of game.crows) c.fillRect(b.x * k - 2, b.y * k - 2, 4, 4);
  c.fillStyle = GOLD;
  circle(0, 0, 3);
  c.fill();
  c.translate(p.x * k, p.y * k);
  c.rotate(p.heading);
  c.fillStyle = '#fff';
  tracePath(6, 0, -4, -4, -4, 4);
  c.fill();
  c.restore();
}

function nameTarget(p) {
  const label = $('label');
  let best = null;
  let bestScore = Infinity;
  if (flying()) {
    for (const n of game.nodes) {
      const dx = n.x - p.x;
      const dy = n.y - p.y;
      const d = Math.hypot(dx, dy);
      if (n.gone || d > LAMP) continue;
      const off = Math.abs(turnBetween(p.heading, Math.atan2(dy, dx)));
      const score = d + off * 260;
      if (off < 0.9 && score < bestScore) {
        best = n;
        bestScore = score;
      }
    }
  }
  label.hidden = !best;
  if (!best) return;
  const tied = best.in.filter(n => !n.gone).length + (best.root >= 0 ? 1 : 0);
  const name = best.count > 1 ? `${best.type} ×${best.count.toLocaleString()}` : best.type;
  label.textContent = `${name} · ${bytes(best.bytes)} · held by ${tied || 'nothing'}`;
  const left = width / 2 + best.x - p.x - label.offsetWidth / 2;
  label.style.left = `${Math.min(innerWidth - label.offsetWidth - 8, Math.max(8, left))}px`;
  label.style.top = `${height / 2 + best.y - p.y - best.radius - 30}px`;
}

function showTimer(now) {
  const seconds = game.deadline === null ? null : Math.max(0, Math.ceil((game.deadline - now) / 1000));
  const padded = String(seconds % 60).padStart(2, '0');
  $('timer').textContent = seconds === null ? '' : `${Math.floor(seconds / 60)}:${padded}`;
  $('timer').classList.toggle('low', seconds !== null && seconds <= 10);
}

let last = performance.now();
function frame(now) {
  const dt = Math.min(0.05, (now - last) / 1000);
  last = now;
  step(dt);
  draw();
  showTimer(now);
  requestAnimationFrame(frame);
}

let socket;
let info = null;
function send(message) {
  if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

function takeState(state) {
  game.lives = state.lives;
  game.maxLives = state.maxLives;
  game.deadline =
    state.remainingMs == null || game.mode !== 'round' ? null : performance.now() + state.remainingMs;
  hud();
}

function showApps() {
  $('apps').replaceChildren(
    ...apps.map(a => {
      const button = Object.assign(document.createElement('button'), {
        textContent: `Freeze ${a.name} and fly`,
      });
      button.title = `process ${a.pid}`;
      button.onclick = () => {
        button.disabled = true;
        send({ type: 'collect', app: a.id });
      };
      return button;
    }),
  );
  if (game.mode === 'wait') {
    const waiting = 'Waiting for an app. Start yours with ./play';
    $('panelText').textContent = apps.length > 0 ? 'Ready when you are.' : waiting;
  }
}

const handlers = {
  apps(message) {
    apps = message.apps;
    showApps();
  },
  snapshot(message) {
    info = message;
    if (message.frozen) send({ type: 'graph' });
  },
  graph(message) {
    if (!info?.frozen || message.gc !== info.gc) return;
    if (message.tooLarge) {
      showPanel(
        'Too crowded to fly',
        `${info.count.toLocaleString()} objects. Use the heap map for this one.`,
        'Practice flight',
      );
      return;
    }
    load(message.nodes, message.edges, info.gc * 7919 + info.pid, message.gone);
    game.mode = 'round';
    resetFlight();
    takeState(info.state);
    $('panel').hidden = true;
    game.disagreements = message.disagreements;
    toast(
      info.outOfMemory
        ? `The app is OUT OF MEMORY. ${bytes(info.bytes)}, ${message.nodes.length} balloons. Make room or it fails!`
        : `Process ${info.pid} is frozen: ${bytes(info.bytes)}, ${message.nodes.length} balloons. Shoot what is loose.`,
    );
  },
  erased(message) {
    if (game.mode !== 'round') return;
    const before = game.lives;
    takeState(message.state);
    for (const h of message.hits) onHit(game.nodes[h.node], h.live, h.bytes, h.count);
    if (game.lives === 0 && before > 0) toast('No lives left. Land with Enter; the app resumes.', true);
  },
  state(message) {
    if (game.mode === 'round') {
      info.state = message.state;
      takeState(message.state);
    }
  },
  resumed(message) {
    if (info) info.frozen = false;
    if (game.mode !== 'round') return;
    game.deadline = null;
    endFlight(
      { done: 'Landed.', time: "Time's up.", gone: 'The app stopped waiting, or exited.' }[message.reason],
      message.missedBytes,
    );
  },
};

function connect() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.onmessage = event => {
    if (typeof event.data !== 'string') return;
    const message = JSON.parse(event.data);
    handlers[message.type]?.(message);
  };
  socket.onclose = () => {
    if (game.mode === 'round') {
      game.mode = 'wait';
      showPanel('Lost the viewer process', 'Retrying.', 'Practice flight');
    }
    setTimeout(connect, 1000);
  };
}

showPanel('Loose Balloons', '', 'Practice flight');
hud();
connect();
requestAnimationFrame(frame);
