// Plays one collection with the mouse: drags over garbage, clicks something alive, presses Done.
import { open, sleep } from './chrome.mjs';
const page = await open(process.argv[2]);
const fail = message => { console.log('FAIL', message); process.exitCode = 1; };
try {
  await page.until('info?.frozen && types && types.length > 0');
  await page.until('!document.getElementById("done").disabled');
  await page.eval('document.getElementById("reveal").click()');
  await page.until('live');
  // Peeking is for the test: find a screen point inside a large dead block, and one on a live block.
  const point = alive => page.eval(`(() => {
    for (let i = 0; i < types.length; i++) {
      if (types[i] === 0 || Boolean(live[i]) !== ${alive} || sizes[i] < unitsPerCell * 3) continue;
      const cell = Math.ceil(offsets[i] / unitsPerCell), x = cell % columns, y = Math.floor(cell / columns);
      const box = map.getBoundingClientRect();
      if (x + 3 < columns && (y + 1) * CELL < map.clientHeight) return { x: box.left + x * CELL + 5, y: box.top + y * CELL + 5, i };
    }
  })()`);

  const dead = await point(false);
  await page.mouse('mouseMoved', dead.x, dead.y);
  await page.mouse('mousePressed', dead.x, dead.y, 1);
  for (let dx = 0; dx <= 20; dx += 5) await page.mouse('mouseMoved', dead.x + dx, dead.y, 1);
  await page.mouse('mouseReleased', dead.x + 20, dead.y);
  await page.until('state.freedCount > 0');
  if (await page.eval(`types[${dead.i}]`) !== 0) fail('the dragged block was not freed');
  if (await page.eval('state.lives') !== 3) fail('erasing garbage cost a life');

  const alive = await point(true);
  await page.mouse('mousePressed', alive.x, alive.y, 1);
  await page.mouse('mouseReleased', alive.x, alive.y);
  await page.until('state.lives === 2');
  if (await page.eval(`types[${alive.i}]`) === 0) fail('a live block was shown as freed');
  await sleep(200);
  await page.screenshot(process.argv[3]);
  console.log(await page.eval('document.getElementById("toast").textContent + " / " + document.getElementById("score").textContent'));

  const done = await page.eval('(() => { const b = document.getElementById("done").getBoundingClientRect(); return { x: b.x + 10, y: b.y + 10 }; })()');
  await page.mouse('mousePressed', done.x, done.y, 1);
  await page.mouse('mouseReleased', done.x, done.y);
  await page.until('!info.frozen');
  if (page.errors.length) fail(page.errors.join('\n'));
  if (!process.exitCode) console.log('ok');
} finally { await page.close(); }
