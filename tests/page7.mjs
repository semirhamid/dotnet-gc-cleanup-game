// Freezes a running app from the flight page, then lands. Arguments: url, screenshot prefix.
import { open, sleep } from './chrome.mjs';
const page = await open(process.argv[2]);
try {
  await page.until('document.querySelectorAll("#apps button").length === 1', 15000);
  await sleep(300);
  await page.screenshot(process.argv[3] + 'ready.png');
  console.log(await page.eval('document.querySelector("#apps button").textContent'));
  await page.eval('document.querySelector("#apps button").click()');
  await page.until('game.mode === "round" && game.nodes.length > 20');
  await sleep(800);
  await page.screenshot(process.argv[3] + 'round.png');
  await page.eval('document.getElementById("land").click()');
  await page.until(
    'game.mode === "over" && document.querySelectorAll("#apps button").length === 1 && !document.querySelector("#apps button").disabled',
  );
  if (page.errors.length) throw new Error(page.errors.join());
  console.log('ok');
} finally {
  await page.close();
}
