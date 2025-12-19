'use strict';
const $ = id => document.getElementById(id);
const map = $('map'), canvas = $('canvas'), context = canvas.getContext('2d');
const CELL = 10, GAP = 1;

let info = null;            // the snapshot message
let offsets, sizes, types;  // Uint32Array, in units of 8 bytes; type 0 is free space
let live = null;            // Uint8Array once revealed
let columns = 1, unitsPerCell = 4;
let isolated = -1;          // type index, or -1 for all
let colors = [];            // per type index

// The eight largest types of the first collection take the palette's slots in order. A type keeps
// its slot for as long as the page is open, so a colour never changes meaning between rounds.
const slots = new Map();

const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function bytes(n) {
  if (n < 1024) return `${n} B`;
  for (const unit of ['KB', 'MB', 'GB']) {
    n /= 1024;
    if (n < 1024 || unit === 'GB') return `${n.toFixed(n < 10 ? 1 : 0)} ${unit}`;
  }
}

// ---- Snapshot ----

function totals() {
  const byType = info.types.map((name, index) => ({ name, index, count: 0, units: 0 }));
  for (let i = 0; i < types.length; i++) {
    byType[types[i]].count++;
    byType[types[i]].units += sizes[i];
  }
  return byType.slice(1).filter(t => t.count > 0).sort((a, b) => b.units - a.units);
}

function applyColors() {
  const ranked = totals();
  for (const t of ranked)
    if (slots.size < 8 && !slots.has(t.name)) slots.set(t.name, slots.size + 1);
  const palette = [css('--other'), ...Array.from({ length: 8 }, (_, i) => css(`--s${i + 1}`))];
  colors = info.types.map(name => palette[slots.get(name) ?? 0]);
  colors[0] = css('--free');

  const legend = $('legend');
  legend.textContent = '';
  for (const t of ranked) {
    const row = document.createElement('button');
    row.setAttribute('aria-pressed', t.index === isolated);
    row.title = t.name;
    const swatch = Object.assign(document.createElement('span'), { className: 'swatch' });
    swatch.style.background = colors[t.index];
    row.append(swatch,
      Object.assign(document.createElement('span'), { className: 'name', textContent: t.name }),
      Object.assign(document.createElement('span'), { className: 'num', textContent: `${t.count.toLocaleString()} · ${bytes(t.units * 8)}` }));
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
  isolated = -1;
  $('reveal').checked = false;
  fillScale();
  applyColors();
  layout();
  updateTitle();
}

function updateTitle() {
  if (!info) return;
  $('title').textContent = `Process ${info.pid}, collection ${info.gc}: ${bytes(info.bytes)} heap`;
  let text = `${info.count.toLocaleString()} objects and free blocks. ` + (info.frozen ? 'The app is paused.' : 'The app has resumed; this is how the heap looked.');
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

// ---- Drawing ----

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
    $('spacer').style.height = `${Math.max(rows * CELL, map.clientHeight)}px`;
    // The canvas is in flow; the spacer only has to make the box scroll.
  }
  draw();
}

// The last block starting at or before a position.
function blockAt(unit) {
  let low = 0, high = offsets.length - 1;
  while (low < high) {
    const middle = (low + high + 1) >> 1;
    if (offsets[middle] <= unit) low = middle; else high = middle - 1;
  }
  return low;
}

function blockColor(i) {
  const type = types[i];
  if (type === 0) return colors[0];
  if (isolated >= 0 && type !== isolated) return css_dim;
  return colors[type];
}

let css_dim = '';
let drawQueued = false;
function draw() {
  if (!drawQueued) {
    drawQueued = true;
    requestAnimationFrame(() => { drawQueued = false; paint(); });
  }
}

function paint() {
  const width = map.clientWidth, height = map.clientHeight, top = map.scrollTop;
  context.clearRect(0, 0, width, height);
  if (!offsets || offsets.length === 0) return;
  css_dim = css('--dim');
  const firstRow = Math.floor(top / CELL), lastRow = Math.ceil((top + height) / CELL);
  const endUnit = lastRow * columns * unitsPerCell;
  for (let i = blockAt(firstRow * columns * unitsPerCell); i < offsets.length && offsets[i] < endUnit; i++) {
    const first = Math.floor(offsets[i] / unitsPerCell);
    const last = Math.max(first, Math.ceil((offsets[i] + sizes[i]) / unitsPerCell) - 1);
    context.fillStyle = blockColor(i);
    // A block is one bar per row, with a gap only where it ends.
    for (let row = Math.max(firstRow, Math.floor(first / columns)); row <= Math.min(lastRow, Math.floor(last / columns)); row++) {
      const from = Math.max(first, row * columns) - row * columns;
      const to = Math.min(last, row * columns + columns - 1) - row * columns;
      context.fillRect(from * CELL, row * CELL - top, (to - from + 1) * CELL - GAP, CELL - GAP);
      if (live && live[i]) {
        // Live blocks carry a mark as well as their colour.
        context.fillStyle = css_text;
        context.fillRect(from * CELL + 3, row * CELL - top + 3, 3, 3);
        context.fillStyle = blockColor(i);
      }
    }
  }
}
let css_text = css('--text');

// ---- Pointer ----

function blockUnder(x, y) {
  if (!offsets || offsets.length === 0) return -1;
  const column = Math.floor(x / CELL), row = Math.floor((y + map.scrollTop) / CELL);
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
  if (live && types[i] !== 0) lines.push(live[i] ? 'alive' : 'dead');
  tip.append(Object.assign(document.createElement('b'), { textContent: info.types[types[i]] }), lines.join(' · '));
  tip.style.left = `${Math.min(event.clientX + 14, innerWidth - tip.offsetWidth - 8)}px`;
  tip.style.top = `${Math.min(event.clientY + 14, innerHeight - tip.offsetHeight - 8)}px`;
}

map.addEventListener('pointermove', showTooltip);
map.addEventListener('pointerleave', () => { $('tooltip').hidden = true; });
map.addEventListener('scroll', draw);
new ResizeObserver(layout).observe(map);
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => { css_text = css('--text'); if (info && types) applyColors(); draw(); });
$('scale').onchange = () => { unitsPerCell = $('scale').value / 8; map.scrollTop = 0; layout(); };
$('reveal').onchange = () => {
  if ($('reveal').checked) send({ type: 'reveal' });
  else { live = null; draw(); }
};

// ---- Connection ----

let socket;
function send(message) { if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message)); }

const handlers = {
  snapshot(message) { info = message; },
  resumed() { if (info) { info.frozen = false; updateTitle(); } },
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
    if (tag === 1) showSnapshot(event.data);
    else if (tag === 2) { live = new Uint8Array(event.data, 8); draw(); }
  };
  socket.onclose = () => {
    $('subtitle').textContent = 'Lost the viewer process. Retrying.';
    setTimeout(connect, 1000);
  };
}
connect();
