// Flies one round of Loose Balloons with real key presses. Positions are set from here, since flying
// well is not what is being tested. Arguments: url, screenshot prefix.
import { open, sleep } from './chrome.mjs';
const page = await open(process.argv[2]);
const fail = message => { console.log('FAIL', message); process.exitCode = 1; };
const key = async (code, ms = 60) => {
  await page.call('Input.dispatchKeyEvent', { type: 'keyDown', code, key: code, windowsVirtualKeyCode: 0 });
  await sleep(ms);
  await page.call('Input.dispatchKeyEvent', { type: 'keyUp', code, key: code, windowsVirtualKeyCode: 0 });
};
// Puts the plane in front of a node with nothing in between, pointing at it, safe from crows.
const aim = which => page.eval(`(() => {
  const alive = reachable().parent;
  const clear = n => { for (let a = 0; a < 6.28; a += 0.2) {
    const x = n.x - Math.cos(a) * 150, y = n.y - Math.sin(a) * 150;
    const blocked = game.nodes.some(m => { if (m === n || m.gone) return false;
      const t = Math.max(0, Math.min(1, ((m.x - x) * (n.x - x) + (m.y - y) * (n.y - y)) / 22500));
      return Math.hypot(x + (n.x - x) * t - m.x, y + (n.y - y) * t - m.y) < m.radius + 22; });
    if (!blocked) return a;
  } return null; };
  for (const n of game.nodes) {
    if (n.gone || !(${which})(n, alive.has(n))) continue;
    const a = clear(n);
    if (a === null) continue;
    Object.assign(game.plane, { x: n.x - Math.cos(a) * 150, y: n.y - Math.sin(a) * 150, heading: a, safeUntil: 1e9, reload: 0 });
    game.crows = []; game.nextCrow = 1e9; keys.clear(); mouseSteers = false;
    return n.id;
  }
  return -1;
})()`);
try {
  await page.until('game.mode === "round"', 20000);
  if (await page.eval('game.disagreements') !== 0) fail('the graph and the GC disagree about what is alive');
  if (!await page.eval('document.getElementById("panel").hidden')) fail('the panel covers the round');

  const loose = await aim('(n, alive) => !alive');
  await key('Space');
  await page.until(`game.nodes[${loose}].gone`);
  if (await page.eval('game.freedBytes') <= 0 || await page.eval('game.points') <= 0) fail('nothing scored');
  if (await page.eval('game.lives') !== 3) fail('shooting garbage cost a life');
  console.log('popped', await page.eval('document.getElementById("freed").textContent'));

  const alive = await aim('(n, alive) => alive');
  await key('Space');
  await page.until('game.lives === 2');
  if (await page.eval(`game.nodes[${alive}].gone`)) fail('a live balloon popped');
  const why = await page.eval('document.getElementById("toast").textContent');
  if (!/^Alive, and a life lost\. Held by a/.test(why)) fail(`explanation reads "${why}"`);
  console.log(why);
  await page.screenshot(process.argv[3] + 'alive.png');

  // A chain shot through garbage only.
  await page.eval('game.chain = 100');
  const before = await page.eval('game.nodes.filter(n => n.gone).length');
  const island = await aim(`(n, alive) => { if (alive || n.out.length === 0) return false;
    const seen = [n]; for (let i = 0; i < seen.length; i++) for (const m of seen[i].out) if (!m.gone && !seen.includes(m)) seen.push(m);
    return seen.length >= 3 && !seen.some(m => reachable().parent.has(m)); }`);
  if (island < 0) fail('no loose island to chain through');
  await key('KeyX');
  await page.until(`game.nodes.filter(n => n.gone).length >= ${before} + 3`);
  if (await page.eval('game.lives') !== 2) fail('the chain hit something alive');
  console.log('chain popped', await page.eval('game.nodes.filter(n => n.gone).length') - before);

  // A flare lights exactly what is reachable.
  await key('KeyF');
  await page.until('game.wave && game.time - game.wave.start > game.wave.end - 2.5');
  await page.eval('Object.assign(game.plane, { x: -260, y: 150, heading: 0 })');
  await sleep(100);
  await page.screenshot(process.argv[3] + 'flare.png');
  if (await page.eval('game.flares') !== 1) fail('the flare was not used up');

  // Crows: three hits wreck the plane.
  for (let i = 0; i < 3; i++) {
    await page.eval('game.plane.safeUntil = 0; game.crows.push({ x: game.plane.x, y: game.plane.y, heading: 0, health: 2, flap: 0 })');
    await sleep(150);
  }
  await page.until('game.lives === 1');

  await key('Enter');
  await page.until('game.mode === "over" && !document.getElementById("panel").hidden');
  console.log(await page.eval('document.getElementById("panelTitle").textContent + " " + document.getElementById("panelText").textContent.replaceAll("\\n", " ")'));
  await page.eval('document.getElementById("go").click()');
  await page.until('game.mode === "practice" && game.nodes.length > 40');
  const practice = await aim('(n, alive) => !alive');
  await key('Space');
  await page.until(`game.nodes[${practice}].gone`);
  if (page.errors.length) fail(page.errors.join('\n'));
  if (!process.exitCode) console.log('ok');
} finally { await page.close(); }
