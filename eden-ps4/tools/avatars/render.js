// Renders characters.js: the atlas the app includes (6 columns of 256x256 cells, clear background),
// and with --preview a sheet of them on their colours, round as the HOME menu shows them.
//   node render.js <atlas.png> [--preview <sheet.png>]
'use strict';
const { chromium } = require('playwright');
const { characters } = require('./characters.js');

const COLUMNS = 6, CELL = 256;
const COLOURS = ['#5AB4F0', '#3D6FE0', '#2CC0B0', '#5CC85C', '#B5DB4C', '#FFD34E', '#FF9A3C', '#F0544F', '#FF8FB8', '#9B6CE0', '#8E8E93', '#3A3A3C'];

function svg(c) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${CELL}" height="${CELL}" viewBox="0 0 256 256">${c.svg}</svg>`;
}

(async () => {
  const [atlas, flag, sheet] = process.argv.slice(2);
  const rows = Math.ceil(characters.length / COLUMNS);
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: COLUMNS * CELL, height: rows * CELL }, deviceScaleFactor: 1 });
  let html = '<html><body style="margin:0;background:transparent"><div style="display:grid;grid-template-columns:repeat(' + COLUMNS + ',' + CELL + 'px)">';
  for (const c of characters) html += `<div style="width:${CELL}px;height:${CELL}px">${svg(c)}</div>`;
  html += '</div></body></html>';
  await page.setContent(html);
  await page.screenshot({ path: atlas, omitBackground: true, clip: { x: 0, y: 0, width: COLUMNS * CELL, height: rows * CELL } });
  if (flag === '--preview') {
    const size = 200, gap = 16;
    const p2 = await browser.newPage({ viewport: { width: COLUMNS * (size + gap) + gap, height: rows * (size + gap + 24) + gap }, deviceScaleFactor: 1 });
    let h = `<html><body style="margin:0;background:#2D2D2D;font:14px sans-serif;color:#ddd"><div style="display:grid;gap:${gap}px;padding:${gap}px;grid-template-columns:repeat(${COLUMNS},${size}px)">`;
    characters.forEach((c, i) => {
      h += `<div><div style="width:${size}px;height:${size}px;border-radius:50%;overflow:hidden;background:${COLOURS[i % COLOURS.length]}">` +
           svg(c).replace(`width="${CELL}" height="${CELL}"`, `width="${size}" height="${size}"`) + `</div><div style="text-align:center">${i + 1} ${c.name}</div></div>`;
    });
    h += '</div></body></html>';
    await p2.setContent(h);
    await p2.screenshot({ path: sheet, fullPage: true });
  }
  await browser.close();
})();
