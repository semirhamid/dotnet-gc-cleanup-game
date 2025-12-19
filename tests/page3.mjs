// Loads the page after a collection and checks that the canvas and legend show it.
import { open, sleep } from './chrome.mjs';
const page = await open(process.argv[2]);
try {
  await page.until('types && types.length > 0');
  await sleep(300);
  const colours = await page.eval(`(() => {
    const d = context.getImageData(0, 0, canvas.width, canvas.height).data, seen = new Set();
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] === 255) seen.add(d[i] << 16 | d[i + 1] << 8 | d[i + 2]);
    return seen.size;
  })()`);
  const rows = await page.eval('document.querySelectorAll("#legend button").length');
  const title = await page.eval('document.getElementById("title").textContent');
  await page.screenshot(process.argv[3]);
  console.log(title, `${colours} colours, ${rows} types`, page.errors);
  if (colours < 8 || rows < 20 || page.errors.length || !title.includes('collection 1')) process.exitCode = 1;
  else console.log('ok');
} finally { await page.close(); }
