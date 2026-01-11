// Checks the game layer on the page. Arguments: url, screenshot, "safe" or "hardcore".
import { open, sleep } from './chrome.mjs';
const hardcore = process.argv[4] === 'hardcore';
const page = await open(process.argv[2]);
const fail = message => {
  console.log('FAIL', message);
  process.exitCode = 1;
};
const visible = id => page.eval(`!document.getElementById("${id}").hidden`);
try {
  await page.until('info?.frozen && state && !document.getElementById("done").disabled');
  const timer = await page.eval('document.getElementById("timer").textContent');
  if (!/^\d+:\d\d$/.test(timer)) fail(`timer reads "${timer}"`);
  if (await visible('danger')) fail('the hardcore warning shows by default');
  if (await page.eval('document.getElementById("hardcore").checked')) fail('hardcore is on by default');
  if ((await visible('hardcoreLabel')) !== hardcore) fail(`hardcore switch visible: ${!hardcore}`);
  if (hardcore) {
    if (!/crashes the app/i.test(await page.eval('document.getElementById("hardcoreLabel").textContent')))
      fail('switch is not labelled');
    await page.eval('document.getElementById("hardcore").click()');
    await page.until('state.hardcore && !document.getElementById("danger").hidden');
    if (!/will crash/i.test(await page.eval('document.getElementById("danger").textContent')))
      fail('warning does not say it crashes');
    await sleep(200);
    await page.screenshot(process.argv[3]);
    await page.eval('document.getElementById("hardcore").click()');
    await page.until('!state.hardcore && document.getElementById("danger").hidden');
  } else {
    // Erase only byte[] objects, by isolating the type and scrubbing the visible map with a big eraser.
    await page.eval(
      '[...document.querySelectorAll("#legend button")].find(b => b.title === "byte[]").click()',
    );
    await page.eval('document.getElementById("radius").value = 8');
    const box = await page.eval(
      '(() => { const b = map.getBoundingClientRect(); return { x: b.x, y: b.y, w: map.clientWidth, h: map.clientHeight }; })()',
    );
    await page.mouse('mousePressed', box.x + 5, box.y + 5, 1);
    for (let y = 5; y < box.h; y += 100) {
      await page.mouse('mouseMoved', box.x + 5, box.y + y, 1);
      await page.mouse('mouseMoved', box.x + box.w - 20, box.y + y, 1);
      await page.mouse('mouseMoved', box.x + 5, box.y + Math.min(y + 50, box.h - 5), 1);
    }
    await page.mouse('mouseReleased', box.x + 5, box.y + box.h - 5);
    await page.until('state.freedCount > 50 && asked.size === 0');
    if ((await page.eval('state.lives')) !== 3) fail('a dimmed block was erased');
    if ((await page.eval('state.score')) !== (await page.eval('state.freedBytes')))
      fail('score is not the bytes freed');
    await sleep(200);
    await page.screenshot(process.argv[3]);
  }
  await page.eval('document.getElementById("done").click()');
  await page.until('!info.frozen && !document.getElementById("summary").hidden');
  console.log(await page.eval('document.getElementById("summary").textContent'));
  if (page.errors.length) fail(page.errors.join('\n'));
  if (!process.exitCode) console.log('ok');
} finally {
  await page.close();
}
