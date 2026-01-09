'use strict';
// The heap map: every object as a bar of blocks, and an eraser to drag over them.
const $ = id => document.getElementById(id);
const map = $('map');
const canvas = $('canvas');
const context = canvas.getContext('2d');
const CELL = 10;
const GAP = 1;
const MAX_BLOCKS_PER_MESSAGE = 4000;

let info = null;
let offsets, sizes, types; // Uint32Array, in units of 8 bytes; type 0 is free space
let live = null; // Uint8Array once revealed
let knownLive = new Set(); // blocks the GC refused to free
let state = null;
let columns = 1;
let unitsPerCell = 4;
let isolated = -1; // type index, or -1 for all
let colors = [];

// Types get the palette's eight colours in the order of their size, until there are none left. A type
// keeps its colour as long as the page is open, so that it looks the same from round to round.
const slots = new Map();

const element = (tag, properties) => Object.assign(document.createElement(tag), properties);
const plural = (n, noun) => `${n.toLocaleString()} ${noun}${n === 1 ? '' : 's'}`;
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function bytes(n) {
  if (n < 1024) return `${n} B`;
  for (const unit of ['KB', 'MB', 'GB']) {
    n /= 1024;
    if (n < 1024 || unit === 'GB') return `${n.toFixed(n < 10 ? 1 : 0)} ${unit}`;
  }
}

function totals() {
  const byType = info.types.map((name, index) => ({ name, index, count: 0, units: 0 }));
  for (let i = 0; i < types.length; i++) {
    byType[types[i]].count++;
    byType[types[i]].units += sizes[i];
  }
  return byType
    .slice(1)
    .filter(t => t.count > 0)
    .sort((a, b) => b.units - a.units);
}

function applyColors() {
  const ranked = totals();
  for (const t of ranked) if (slots.size < 8 && !slots.has(t.name)) slots.set(t.name, slots.size + 1);
  const palette = [css('--other'), ...Array.from({ length: 8 }, (_, i) => css(`--s${i + 1}`))];
  colors = info.types.map(name => palette[slots.get(name) ?? 0]);
  colors[0] = css('--free');

  fillLegend(ranked);
}

function fillLegend(ranked) {
  if (isolated >= 0 && !ranked.some(t => t.index === isolated)) isolated = -1;
  const legend = $('legend');
  legend.textContent = '';
  for (const t of ranked) {
    const row = element('button', { title: t.name });
    row.setAttribute('aria-pressed', t.index === isolated);
    const swatch = element('span', { className: 'swatch' });
    swatch.style.background = colors[t.index];
    row.append(
      swatch,
      element('span', { className: 'name', textContent: t.name }),
      element('span', {
        className: 'num',
        textContent: `${t.count.toLocaleString()} · ${bytes(t.units * 8)}`,
      }),
    );
    row.onclick = () => {
      isolated = isolated === t.index ? -1 : t.index;
      for (const other of legend.children) other.setAttribute('aria-pressed', other === row && isolated >= 0);
      draw();
    };
    legend.append(row);
  }
}

function showSnapshot(frame) {
  const words = new Uint32Array(frame);
  const n = words[1];
  offsets = words.slice(2, 2 + n);
  sizes = words.slice(2 + n, 2 + 2 * n);
  types = words.slice(2 + 2 * n, 2 + 3 * n);
  live = null;
  knownLive = new Set(info.knownLive);
  isolated = -1;
  asked.clear();
  $('reveal').checked = false;
  fillScale();
  applyColors();
  layout();
  updateTitle();
  updateState(info.state);
}

function updateState(next) {
  state = next;
  const lives = $('lives');
  lives.textContent = '';
  lives.setAttribute('aria-label', `${state.lives} of ${state.maxLives} lives`);
  lives.append(
    '♥'.repeat(state.lives),
    element('span', { className: 'lost', textContent: '♡'.repeat(state.maxLives - state.lives) }),
  );
  const score = [
    `Score ${state.score.toLocaleString()}${state.peeked ? ' (peeked)' : ''}`,
    `total ${state.totalScore.toLocaleString()}`,
    `${bytes(state.freedBytes)} freed from ${plural(state.freedCount, 'object')}`,
  ];
  if (state.destroyed) score.push(`${state.destroyed.toLocaleString()} live destroyed`);
  $('score').textContent = score.join(' · ');
  deadline = info.frozen && state.remainingMs != null ? performance.now() + state.remainingMs : null;
  showTimer();
  $('hardcoreLabel').hidden = !(info.frozen && info.hardcoreAllowed);
  $('hardcore').checked = state.hardcore;
  $('danger').hidden = !(info.frozen && state.hardcore);
  $('done').disabled = !info.frozen;
  map.classList.toggle('playing', canErase());
}

const canErase = () => info?.frozen && (state?.lives > 0 || state?.hardcore);

let deadline = null;
function showTimer() {
  const seconds = deadline === null ? null : Math.max(0, Math.ceil((deadline - performance.now()) / 1000));
  const padded = String(seconds % 60).padStart(2, '0');
  $('timer').textContent = seconds === null ? '' : `${Math.floor(seconds / 60)}:${padded}`;
  $('timer').classList.toggle('low', seconds !== null && seconds <= 10);
}
setInterval(showTimer, 250);

function showSummary(message) {
  const reasons = { done: 'Round over.', time: "Time's up.", gone: 'The app stopped waiting, or exited.' };
  const why = reasons[message.reason];
  let missed = 'Not a byte of garbage was left.';
  if (message.missedBytes > 0) missed = `${bytes(message.missedBytes)} of garbage was left behind.`;
  $('summary').textContent = `${why} Score ${state.score.toLocaleString()}. ${missed}`;
  $('summary').hidden = false;
}

let toastTimer;
function toast(text) {
  $('toast').textContent = text;
  $('toast').hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    $('toast').hidden = true;
  }, 2500);
}

function updateTitle() {
  if (!info) return;
  $('title').textContent = `Process ${info.pid}, collection ${info.gc}: ${bytes(info.bytes)} heap`;
  let text = `${info.count.toLocaleString()} objects and free blocks. `;
  if (!info.frozen) text += 'The app has resumed; this is how the heap looked.';
  else if (info.outOfMemory) text += 'The app is out of memory! Make room.';
  else text += 'The app is paused.';
  if (info.shown < info.count) text += ` Only the first ${info.shown.toLocaleString()} are shown.`;
  $('subtitle').textContent = text;
}

function fillScale() {
  const select = $('scale');
  const visible = Math.max(1, Math.floor(map.clientWidth / CELL) * Math.floor(map.clientHeight / CELL));
  // Start with the heap filling about two screens.
  let best = 8;
  while (best < 65536 && info.bytes / best > visible * 2) best *= 2;
  select.textContent = '';
  for (let b = 8; b <= 65536; b *= 2) select.append(new Option(bytes(b), b, b === best, b === best));
  unitsPerCell = best / 8;
}

function layout() {
  const ratio = devicePixelRatio || 1;
  canvas.width = map.clientWidth * ratio;
  canvas.height = map.clientHeight * ratio;
  canvas.style.width = `${map.clientWidth}px`;
  canvas.style.height = `${map.clientHeight}px`;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  columns = Math.max(1, Math.floor(map.clientWidth / CELL));
  if (offsets) {
    const rows = Math.ceil(info.bytes / 8 / unitsPerCell / columns);
    // The canvas stays in view. The spacer is what makes the box scroll.
    $('spacer').style.height = `${Math.max(rows * CELL, map.clientHeight)}px`;
  }
  draw();
}

// The last block starting at or before a position.
function blockAt(unit) {
  let low = 0;
  let high = offsets.length - 1;
  while (low < high) {
    const middle = (low + high + 1) >> 1;
    if (offsets[middle] <= unit) low = middle;
    else high = middle - 1;
  }
  return low;
}

let dimColor = '';
let textColor = css('--text');

function blockColor(i) {
  const type = types[i];
  if (type === 0) return colors[0];
  if (isolated >= 0 && type !== isolated) return dimColor;
  return colors[type];
}

let drawQueued = false;
function draw() {
  if (!drawQueued) {
    drawQueued = true;
    requestAnimationFrame(() => {
      drawQueued = false;
      paint();
    });
  }
}

function paint() {
  const width = map.clientWidth;
  const height = map.clientHeight;
  const top = map.scrollTop;
  context.clearRect(0, 0, width, height);
  if (!offsets || offsets.length === 0) return;
  dimColor = css('--dim');
  const firstRow = Math.floor(top / CELL);
  const lastRow = Math.ceil((top + height) / CELL);
  const endUnit = lastRow * columns * unitsPerCell;
  for (let i = blockAt(firstRow * columns * unitsPerCell); i < offsets.length && offsets[i] < endUnit; i++) {
    const first = Math.floor(offsets[i] / unitsPerCell);
    const last = Math.max(first, Math.ceil((offsets[i] + sizes[i]) / unitsPerCell) - 1);
    context.fillStyle = blockColor(i);
    // A block is one bar per row, with a gap only where it ends.
    const startRow = Math.max(firstRow, Math.floor(first / columns));
    const endRow = Math.min(lastRow, Math.floor(last / columns));
    for (let row = startRow; row <= endRow; row++) {
      const from = Math.max(first, row * columns) - row * columns;
      const to = Math.min(last, row * columns + columns - 1) - row * columns;
      context.fillRect(from * CELL, row * CELL - top, (to - from + 1) * CELL - GAP, CELL - GAP);
      if (knownLive.has(i) || (live && live[i])) {
        context.fillStyle = textColor;
        context.fillRect(from * CELL + 3, row * CELL - top + 3, 3, 3);
        context.fillStyle = blockColor(i);
      }
    }
  }
}

function blockUnder(x, y) {
  if (!offsets || offsets.length === 0) return -1;
  const column = Math.floor(x / CELL);
  const row = Math.floor((y + map.scrollTop) / CELL);
  if (column < 0 || column >= columns || row < 0) return -1;
  const unit = (row * columns + column) * unitsPerCell;
  if (unit >= info.bytes / 8) return -1;
  return blockAt(unit + unitsPerCell - 1);
}

function showTooltip(event) {
  const box = map.getBoundingClientRect();
  const i = blockUnder(event.clientX - box.left, event.clientY - box.top);
  const tip = $('tooltip');
  tip.hidden = i < 0;
  if (i < 0) return;
  tip.textContent = '';
  const address = BigInt(info.heapBase) + BigInt(offsets[i]) * 8n;
  const lines = [`${(sizes[i] * 8).toLocaleString()} bytes at 0x${address.toString(16)}`];
  if (knownLive.has(i)) lines.push('alive');
  else if (live && types[i] !== 0) lines.push(live[i] ? 'alive' : 'dead');
  tip.append(element('b', { textContent: info.types[types[i]] }), lines.join(' · '));
  tip.style.left = `${Math.min(event.clientX + 14, innerWidth - tip.offsetWidth - 8)}px`;
  tip.style.top = `${Math.min(event.clientY + 14, innerHeight - tip.offsetHeight - 8)}px`;
}

let legendQueued = false;
function refreshLegend() {
  if (legendQueued) return;
  legendQueued = true;
  setTimeout(() => {
    legendQueued = false;
    fillLegend(totals());
    draw();
  }, 200);
}

const asked = new Set(); // sent and not answered yet, so that a drag does not send them again
let erasing = false;
let pending = [];
let lastPoint = null;

function eraseAt(x, y) {
  const r = Number($('radius').value);
  for (let dy = -r; dy <= r; dy++) {
    for (let dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r * r) continue;
      const i = blockUnder(x + dx * CELL, y + dy * CELL);
      if (i < 0 || types[i] === 0 || asked.has(i)) continue;
      if (knownLive.has(i) && !state.hardcore) continue;
      if (isolated >= 0 && types[i] !== isolated) continue;
      asked.add(i);
      pending.push(i);
    }
  }
}

// Fast drags skip pixels, so fill in the line between two pointer events.
function eraseTo(event) {
  const box = map.getBoundingClientRect();
  const x = event.clientX - box.left;
  const y = event.clientY - box.top;
  const from = lastPoint ?? { x, y };
  const steps = Math.max(1, Math.ceil(Math.hypot(x - from.x, y - from.y) / (CELL / 2)));
  for (let s = 1; s <= steps; s++) {
    eraseAt(from.x + ((x - from.x) * s) / steps, from.y + ((y - from.y) * s) / steps);
  }
  lastPoint = { x, y };
}

function flushErase() {
  if (pending.length > 0) {
    send({ type: 'erase', blocks: pending.slice(0, MAX_BLOCKS_PER_MESSAGE) });
    pending = pending.slice(MAX_BLOCKS_PER_MESSAGE);
  }
  if (erasing || pending.length > 0) requestAnimationFrame(flushErase);
}

map.addEventListener('pointerdown', event => {
  const box = map.getBoundingClientRect();
  const onScrollbar = event.clientX - box.left >= map.clientWidth;
  if (event.button !== 0 || !canErase() || onScrollbar) return;
  map.setPointerCapture(event.pointerId);
  erasing = true;
  lastPoint = null;
  eraseTo(event);
  requestAnimationFrame(flushErase);
});
map.addEventListener('pointermove', event => {
  if (erasing && canErase()) eraseTo(event);
});
for (const name of ['pointerup', 'pointercancel'])
  map.addEventListener(name, () => {
    erasing = false;
  });
$('hardcore').onchange = () => {
  asked.clear();
  send({ type: 'hardcore', on: $('hardcore').checked });
};
$('done').onclick = () => {
  $('done').disabled = true;
  send({ type: 'done' });
};

map.addEventListener('pointermove', showTooltip);
map.addEventListener('pointerleave', () => {
  $('tooltip').hidden = true;
});
map.addEventListener('scroll', draw);
new ResizeObserver(layout).observe(map);
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  textColor = css('--text');
  if (info && types) applyColors();
  draw();
});
$('scale').onchange = () => {
  unitsPerCell = $('scale').value / 8;
  map.scrollTop = 0;
  layout();
};
$('reveal').onchange = () => {
  if ($('reveal').checked) {
    send({ type: 'reveal' });
  } else {
    live = null;
    draw();
  }
};

let socket;
function send(message) {
  if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

let apps = [];
function showApps() {
  $('apps').replaceChildren(
    ...apps.map(a => {
      const button = element('button', {
        textContent: `Freeze ${a.name} (${a.pid})`,
        disabled: Boolean(info?.frozen),
      });
      button.onclick = () => send({ type: 'collect', app: a.id });
      return button;
    }),
  );
  if (!info && apps.length > 0) {
    $('title').textContent = 'Ready';
    $('subtitle').textContent = 'Freeze an app to see its heap.';
  }
}

const handlers = {
  apps(message) {
    apps = message.apps;
    showApps();
  },
  snapshot(message) {
    info = message;
    $('summary').hidden = true;
    showApps();
  },
  resumed(message) {
    if (!info) return;
    info.frozen = false;
    showApps();
    erasing = false;
    updateTitle();
    updateState(message.state);
    showSummary(message);
  },
  state(message) {
    updateState(message.state);
    draw();
  },
  erased(message) {
    for (const i of message.freed) {
      types[i] = 0;
      asked.delete(i);
      if (live) live[i] = 0;
    }
    for (const i of message.destroyed) {
      types[i] = 0;
      asked.delete(i);
      knownLive.delete(i);
      if (live) live[i] = 0;
    }
    for (const i of message.live) {
      knownLive.add(i);
      asked.delete(i);
    }
    const before = state.lives;
    updateState(message.state);
    if (message.live.length > 0) {
      const name = info.types[types[message.live[0]]];
      const lost = before - state.lives;
      let cost = lost === 1 ? 'One life lost.' : `${lost} lives lost.`;
      if (state.lives === 0) cost = 'No lives left, so press Done.';
      toast(`${name} is in use. ${cost}`);
      pending = [];
    }
    refreshLegend();
    draw();
  },
};

function connect() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.binaryType = 'arraybuffer';
  socket.onmessage = event => {
    if (typeof event.data === 'string') {
      const message = JSON.parse(event.data);
      handlers[message.type]?.(message);
      return;
    }
    const tag = new Uint32Array(event.data, 0, 1)[0];
    if (tag === 1) {
      showSnapshot(event.data);
    } else if (tag === 2) {
      live = new Uint8Array(event.data, 8);
      draw();
    }
  };
  socket.onclose = () => {
    $('subtitle').textContent = 'Lost the viewer process. Retrying.';
    setTimeout(connect, 1000);
  };
}
connect();
