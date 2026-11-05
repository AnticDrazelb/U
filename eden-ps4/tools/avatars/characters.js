// The user icons' characters: original art, each drawn in a 256x256 box on a clear background (the
// user's chosen colour goes behind it). render.js turns them into ../../ui-art/avatars.png.
// Everything that's in a circle centred in the box shows when the icon is drawn round.

'use strict';

const INK = '#2B2230';

function shade(hex, f) {
  // f < 0 darker, > 0 lighter
  const n = parseInt(hex.slice(1), 16);
  let r = n >> 16, g = (n >> 8) & 255, b = n & 255;
  const t = f < 0 ? 0 : 255, p = Math.abs(f);
  r = Math.round((t - r) * p + r); g = Math.round((t - g) * p + g); b = Math.round((t - b) * p + b);
  return '#' + ((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1);
}

// two shiny eyes at cx +- dx
function eyes(cx, cy, dx, r = 9, color = INK, shine = '#FFFFFF') {
  let s = '';
  for (const x of [cx - dx, cx + dx]) {
    s += `<ellipse cx="${x}" cy="${cy}" rx="${r}" ry="${r * 1.18}" fill="${color}"/>`;
    s += `<circle cx="${x + r * 0.32}" cy="${cy - r * 0.42}" r="${r * 0.4}" fill="${shine}"/>`;
    s += `<circle cx="${x - r * 0.3}" cy="${cy + r * 0.45}" r="${r * 0.16}" fill="${shine}" opacity="0.8"/>`;
  }
  return s;
}
// eyes with a coloured iris (for dark faces)
function irisEyes(cx, cy, dx, r, iris, pupil = INK) {
  let s = '';
  for (const x of [cx - dx, cx + dx]) {
    s += `<ellipse cx="${x}" cy="${cy}" rx="${r}" ry="${r * 1.15}" fill="${iris}"/>`;
    s += `<ellipse cx="${x}" cy="${cy + r * 0.05}" rx="${r * 0.55}" ry="${r * 0.85}" fill="${pupil}"/>`;
    s += `<circle cx="${x + r * 0.3}" cy="${cy - r * 0.4}" r="${r * 0.32}" fill="#FFFFFF"/>`;
  }
  return s;
}
function blush(cx, cy, dx, color = '#FF7A93', opacity = 0.45, rx = 13, ry = 8) {
  return `<ellipse cx="${cx - dx}" cy="${cy}" rx="${rx}" ry="${ry}" fill="${color}" opacity="${opacity}"/>` +
         `<ellipse cx="${cx + dx}" cy="${cy}" rx="${rx}" ry="${ry}" fill="${color}" opacity="${opacity}"/>`;
}
// a cat's "w" mouth under the nose
function catMouth(cx, cy, w = 9, color = INK) {
  return `<path d="M${cx - w * 1.9} ${cy - 1} q${w * 0.95} ${w * 0.95} ${w * 1.9} 0 q${w * 0.95} ${w * 0.95} ${w * 1.9} 0" ` +
         `fill="none" stroke="${color}" stroke-width="3.6" stroke-linecap="round" stroke-linejoin="round"/>`;
}
function smile(cx, cy, w = 12, color = INK, width = 3.6) {
  return `<path d="M${cx - w} ${cy} q${w} ${w * 0.9} ${w * 2} 0" fill="none" stroke="${color}" ` +
         `stroke-width="${width}" stroke-linecap="round"/>`;
}
function openSmile(cx, cy, w = 12, tongue = '#FF7B8A') {
  return `<path d="M${cx - w} ${cy} q${w} ${w * 1.5} ${w * 2} 0 z" fill="${INK}"/>` +
         `<path d="M${cx - w * 0.55} ${cy + w * 0.55} q${w * 0.55} ${w * 0.6} ${w * 1.1} 0 q${-w * 0.55} ${-w * 0.42} ${-w * 1.1} 0z" fill="${tongue}"/>`;
}
function triNose(cx, cy, w = 7, color = '#F28AA0') {
  return `<path d="M${cx - w} ${cy - w * 0.55} h${w * 2} q0 ${w * 0.4} ${-w} ${w * 1.15} q${-w} ${-w * 0.75} ${-w} ${-w * 1.15}z" ` +
         `fill="${color}" stroke="${color}" stroke-width="2" stroke-linejoin="round"/>`;
}
function whiskers(cx, cy, dx, color = '#FFFFFF', opacity = 0.9) {
  let s = '';
  for (const side of [-1, 1]) {
    const x0 = cx + side * dx, x1 = cx + side * (dx + 34);
    s += `<path d="M${x0} ${cy - 4} L${x1} ${cy - 10} M${x0} ${cy + 4} L${x1} ${cy + 6}" stroke="${color}" ` +
         `stroke-width="3" stroke-linecap="round" opacity="${opacity}"/>`;
  }
  return s;
}
// the shoulders at the bottom of the box
function body(color, extra = '') {
  return `<ellipse cx="128" cy="300" rx="98" ry="96" fill="${color}"/>` + extra;
}
// a soft highlight on a round head
function gloss(cx, cy, r) {
  return `<ellipse cx="${cx - r * 0.38}" cy="${cy - r * 0.5}" rx="${r * 0.36}" ry="${r * 0.2}" ` +
         `transform="rotate(-28 ${cx - r * 0.38} ${cy - r * 0.5})" fill="#FFFFFF" opacity="0.18"/>`;
}
// a rounded triangle ear from its tip and base corners
function ear(tip, a, b, fill) {
  const [tx, ty] = tip, [ax, ay] = a, [bx, by] = b;
  return `<path d="M${ax} ${ay} Q${(ax + tx) / 2 - 2} ${(ay + ty) / 2 - 4} ${tx} ${ty} Q${(bx + tx) / 2 + 2} ${(by + ty) / 2 - 4} ${bx} ${by} Z" ` +
         `fill="${fill}" stroke="${fill}" stroke-width="6" stroke-linejoin="round"/>`;
}
function pointyEars(fill, inner, spread = 58, top = 36, base = 96, width = 30) {
  let s = '';
  for (const side of [-1, 1]) {
    const tx = 128 + side * spread, bx0 = 128 + side * (spread - width - 4), bx1 = 128 + side * (spread + width - 6);
    s += ear([tx, top], [bx0, base], [bx1, base + 16], fill);
    s += ear([tx + side * 1, top + 18], [bx0 + side * 10, base], [bx1 - side * 10, base + 10], inner);
  }
  return s;
}
function roundEars(fill, inner, dx = 60, cy = 66, r = 26, ri = 14) {
  let s = '';
  for (const side of [-1, 1]) {
    s += `<circle cx="${128 + side * dx}" cy="${cy}" r="${r}" fill="${fill}"/>`;
    if (inner) s += `<circle cx="${128 + side * dx}" cy="${cy + 3}" r="${ri}" fill="${inner}"/>`;
  }
  return s;
}
function head(fill, cx = 128, cy = 132, rx = 80, ry = 74) {
  return `<ellipse cx="${cx}" cy="${cy}" rx="${rx}" ry="${ry}" fill="${fill}"/>`;
}
// shapes cut to the head's ellipse
let clipCount = 0;
function clipped(inner, cx = 128, cy = 132, rx = 80, ry = 74) {
  const id = 'c' + (clipCount++);
  return `<clipPath id="${id}"><ellipse cx="${cx}" cy="${cy}" rx="${rx}" ry="${ry}"/></clipPath><g clip-path="url(#${id})">${inner}</g>`;
}

const characters = [];
function add(name, svg) { characters.push({ name, svg }); }

// 1 ginger cat
{
  const c = '#F6A445', d = '#DE7F2A';
  add('Ginger Cat', body(c) + pointyEars(c, '#FFC7B5') + head(c) +
      clipped(`<rect x="114" y="54" width="8" height="26" rx="4" fill="${d}"/><rect x="128" y="52" width="8" height="30" rx="4" fill="${d}" transform="translate(-4 0)"/><rect x="138" y="54" width="8" height="26" rx="4" fill="${d}"/>` +
              `<path d="M48 124 h24 M46 140 h22" stroke="${d}" stroke-width="7" stroke-linecap="round"/><path d="M208 124 h-24 M210 140 h-22" stroke="${d}" stroke-width="7" stroke-linecap="round"/>`) +
      `<ellipse cx="128" cy="164" rx="34" ry="24" fill="#FFF1DF"/>` + gloss(128, 132, 80) +
      eyes(128, 128, 32) + triNose(128, 152) + catMouth(128, 160) + blush(128, 160, 50) + whiskers(128, 158, 44, '#FFFFFF', 0.7));
}
// 2 black cat
{
  const c = '#38323F';
  add('Black Cat', body(c, `<path d="M108 226 q20 14 40 0" stroke="#E8475A" stroke-width="10" fill="none" stroke-linecap="round"/><circle cx="128" cy="240" r="9" fill="#F7C948"/>`) +
      pointyEars(c, '#E58FA8') + head(c) + gloss(128, 132, 80) +
      irisEyes(128, 128, 32, 13, '#C9E34B') + triNose(128, 154, 6, '#F08AA4') + catMouth(128, 161, 8, '#FFFFFF') +
      blush(128, 160, 50, '#FF7A93', 0.35) + whiskers(128, 158, 44, '#FFFFFF', 0.55));
}
// 3 calico
{
  const c = '#FFF8F0';
  add('Calico', body(c) +
      ear([70, 36], [94, 96], [44, 110], '#F29A47') + ear([71, 54], [86, 96], [56, 104], '#FFC7B5') +
      ear([186, 36], [162, 96], [212, 110], '#3E3843') + ear([185, 54], [170, 96], [200, 104], '#E58FA8') +
      head(c) + clipped(`<ellipse cx="74" cy="88" rx="50" ry="46" fill="#F29A47"/><ellipse cx="190" cy="80" rx="34" ry="28" fill="#3E3843"/><ellipse cx="200" cy="170" rx="18" ry="14" fill="#F29A47"/>`) +
      gloss(128, 132, 80) + eyes(128, 128, 32) + triNose(128, 152) + catMouth(128, 160) + blush(128, 160, 50) +
      whiskers(128, 158, 44, '#C9B9AE', 0.8));
}
// 4 shiba
{
  const c = '#EB9440', w = '#FFF6EA';
  add('Shiba', body(c, `<ellipse cx="128" cy="258" rx="50" ry="40" fill="${w}"/>`) + pointyEars(c, w, 56, 40, 98, 28) + head(c) +
      clipped(`<ellipse cx="88" cy="168" rx="44" ry="36" fill="${w}"/><ellipse cx="168" cy="168" rx="44" ry="36" fill="${w}"/><ellipse cx="128" cy="180" rx="40" ry="30" fill="${w}"/>`) +
      `<ellipse cx="98" cy="104" rx="9" ry="6" fill="${w}"/><ellipse cx="158" cy="104" rx="9" ry="6" fill="${w}"/>` +
      gloss(128, 132, 80) + eyes(128, 128, 32) + `<ellipse cx="128" cy="152" rx="12" ry="9" fill="${INK}"/><circle cx="124" cy="149" r="3" fill="#FFF" opacity=".7"/>` +
      openSmile(128, 164, 11) + blush(128, 160, 52));
}
// 5 floppy-eared dog
{
  const c = '#F3DFC1', e = '#8E5B3C';
  add('Puppy', body(c, `<path d="M96 222 h64 l-6 14 h-52z" fill="#3D7BE0"/><circle cx="128" cy="240" r="8" fill="#F7C948"/>`) +
      head(c) +
      `<ellipse cx="56" cy="128" rx="24" ry="52" transform="rotate(16 56 128)" fill="${e}"/><ellipse cx="200" cy="128" rx="24" ry="52" transform="rotate(-16 200 128)" fill="${e}"/>` +
      clipped(`<ellipse cx="164" cy="110" rx="26" ry="24" fill="#E1B98C"/>`) +
      `<ellipse cx="128" cy="166" rx="36" ry="26" fill="#FFF8EE"/>` + gloss(128, 132, 80) + eyes(128, 126, 30) +
      `<ellipse cx="128" cy="152" rx="13" ry="9" fill="${INK}"/><circle cx="124" cy="149" r="3" fill="#FFF" opacity=".7"/>` +
      openSmile(128, 165, 11) + blush(128, 160, 50));
}
// 6 bear
{
  const c = '#A86C46';
  add('Bear', body(c) + roundEars(c, '#D9A27C', 60, 66, 26, 14) + head(c) + gloss(128, 132, 80) +
      `<ellipse cx="128" cy="166" rx="34" ry="26" fill="#E9C6A1"/>` + eyes(128, 126, 32) +
      `<ellipse cx="128" cy="156" rx="12" ry="8.5" fill="${INK}"/><circle cx="124" cy="153" r="3" fill="#FFF" opacity=".7"/>` +
      smile(122, 168, 6) + smile(134, 168, 6) + blush(128, 156, 52));
}
// 7 panda
{
  const c = '#FBFBFB', k = '#2E2A33';
  let patches = '';
  for (const side of [-1, 1]) patches += `<ellipse cx="${128 + side * 32}" cy="${130}" rx="20" ry="26" transform="rotate(${side * -30} ${128 + side * 32} 130)" fill="${k}"/>`;
  add('Panda', body(k, `<ellipse cx="128" cy="272" rx="52" ry="40" fill="${c}"/>`) + roundEars(k, null, 62, 64, 26) + head(c) + gloss(128, 132, 80) +
      patches + eyes(128, 128, 32, 8, '#FFFFFF', k) +
      `<ellipse cx="128" cy="160" rx="11" ry="7.5" fill="${k}"/>` + smile(122, 168, 6) + smile(134, 168, 6) + blush(128, 160, 54));
}
// 8 polar bear
{
  const c = '#F4F7FB';
  add('Polar Bear', body(c, `<path d="M84 222 q44 26 88 0 l4 22 q-48 22 -96 0z" fill="#4AA3DF"/>`) + roundEars(c, '#DCE4EE', 62, 70, 22, 11) + head(c) +
      clipped(`<ellipse cx="128" cy="220" rx="90" ry="40" fill="#DCE5F0"/>`) + gloss(128, 132, 80) +
      `<ellipse cx="128" cy="164" rx="32" ry="24" fill="#FFFFFF"/>` + eyes(128, 126, 30, 8) +
      `<ellipse cx="128" cy="155" rx="12" ry="8" fill="${INK}"/>` + smile(122, 166, 6) + smile(134, 166, 6) + blush(128, 154, 50));
}
// 9 koala
{
  const c = '#A9ACB8';
  let ears = '';
  for (const side of [-1, 1]) {
    ears += `<circle cx="${128 + side * 70}" cy="80" r="36" fill="${c}"/><circle cx="${128 + side * 72}" cy="84" r="22" fill="#F0DEE2"/>`;
  }
  add('Koala', body(c, `<ellipse cx="128" cy="268" rx="44" ry="36" fill="#E4E6EC"/>`) + ears + head(c, 128, 136, 76, 72) + gloss(128, 136, 76) +
      eyes(128, 128, 34, 8) + `<ellipse cx="128" cy="152" rx="16" ry="22" fill="#3A3744"/><ellipse cx="122" cy="142" rx="4" ry="6" fill="#FFF" opacity=".35"/>` +
      smile(128 - 9, 180, 9, INK, 3.4) + blush(128, 164, 52));
}
// 10 rabbit
{
  const c = '#F6F2EF', p = '#F6B4C4';
  add('Rabbit', body(c, `<path d="M110 222 l18 12 l18 -12 l0 26 l-18 -12 l-18 12z" fill="#E8475A"/>`) +
      `<ellipse cx="96" cy="58" rx="20" ry="54" transform="rotate(-10 96 58)" fill="${c}"/><ellipse cx="96" cy="62" rx="10" ry="40" transform="rotate(-10 96 62)" fill="${p}"/>` +
      `<ellipse cx="160" cy="58" rx="20" ry="54" transform="rotate(10 160 58)" fill="${c}"/><ellipse cx="160" cy="62" rx="10" ry="40" transform="rotate(10 160 62)" fill="${p}"/>` +
      head(c, 128, 146, 74, 66) + clipped(`<ellipse cx="128" cy="222" rx="90" ry="40" fill="#E9E1DC"/>`, 128, 146, 74, 66) + gloss(128, 146, 74) +
      eyes(128, 140, 30, 8.5) + triNose(128, 162, 6, '#F28AA0') + catMouth(128, 169, 7) + blush(128, 168, 48));
}
// 11 fox
{
  const c = '#F28A3B', w = '#FFF5EA', k = '#4A3328';
  let ears = '';
  for (const side of [-1, 1]) {
    const tx = 128 + side * 60;
    ears += ear([tx, 30], [128 + side * 22, 100], [128 + side * 90, 110], c);
    ears += `<path d="M${tx} 32 L${tx - side * 12} 56 L${tx + side * 14} 58 Z" fill="${k}" stroke="${k}" stroke-width="6" stroke-linejoin="round"/>`;
    ears += ear([tx + side * 1, 58], [128 + side * 36, 100], [128 + side * 78, 104], w);
  }
  add('Fox', body(c, `<ellipse cx="128" cy="262" rx="46" ry="42" fill="${w}"/>`) + ears + head(c) +
      clipped(`<path d="M40 140 Q90 130 128 196 Q166 130 216 140 L216 230 L40 230 Z" fill="${w}"/>`) + gloss(128, 132, 80) +
      eyes(128, 128, 32) + `<ellipse cx="128" cy="164" rx="11" ry="8" fill="${INK}"/>` + smile(121, 175, 7) + blush(128, 160, 54));
}
// 12 raccoon
{
  const c = '#9E9CA6', k = '#4A4652', w = '#F2EFF2';
  add('Raccoon', body(c) + pointyEars(c, k, 58, 44, 98, 26) + head(c) +
      clipped(`<path d="M44 126 Q70 104 104 118 Q128 128 152 118 Q186 104 212 126 Q206 152 180 150 Q154 146 128 146 Q102 146 76 150 Q50 152 44 126Z" fill="${k}"/>` +
              `<ellipse cx="128" cy="96" rx="12" ry="22" fill="${w}"/><ellipse cx="128" cy="176" rx="40" ry="28" fill="${w}"/>`) +
      gloss(128, 132, 80) + eyes(128, 130, 32, 8.5, '#FFFFFF', k) +
      `<ellipse cx="128" cy="160" rx="11" ry="8" fill="${INK}"/>` + smile(122, 170, 6) + smile(134, 170, 6));
}
// 13 mouse
{
  const c = '#B9B4C2';
  add('Mouse', body(c) + roundEars(c, '#F4B6C8', 70, 70, 40, 26) + head(c, 128, 142, 74, 68) + gloss(128, 142, 74) +
      eyes(128, 138, 30, 8.5) + `<ellipse cx="128" cy="162" rx="9" ry="7" fill="#F28AA0"/>` + catMouth(128, 170, 7) +
      `<rect x="122" y="173" width="12" height="9" rx="2" fill="#FFFFFF" stroke="${INK}" stroke-width="2"/>` +
      blush(128, 166, 48) + whiskers(128, 164, 38, '#6E6878', 0.6));
}
// 14 hamster
{
  const c = '#F0B765', w = '#FFF4E2';
  add('Hamster', body(c, `<ellipse cx="128" cy="268" rx="56" ry="44" fill="${w}"/>`) + roundEars(c, '#F6B3B8', 58, 74, 20, 11) +
      head(c, 128, 140, 84, 72) + clipped(`<ellipse cx="128" cy="186" rx="66" ry="46" fill="${w}"/>`, 128, 140, 84, 72) +
      gloss(128, 140, 84) + eyes(128, 134, 36, 8) + `<ellipse cx="128" cy="156" rx="7" ry="5" fill="#F28AA0"/>` + catMouth(128, 163, 6) +
      blush(128, 166, 56, '#FF8A9B', 0.55, 16, 10));
}
// 15 pig
{
  const c = '#F9B4C2', d = '#EE8DA4';
  add('Piglet', body(c) + ear([84, 50], [66, 100], [112, 82], d) + ear([172, 50], [190, 100], [144, 82], d) + head(c) + gloss(128, 132, 80) +
      eyes(128, 122, 34, 8) + `<ellipse cx="128" cy="156" rx="28" ry="20" fill="${d}"/><ellipse cx="118" cy="156" rx="5" ry="7" fill="#B95A74"/><ellipse cx="138" cy="156" rx="5" ry="7" fill="#B95A74"/>` +
      smile(120, 184, 8) + blush(128, 150, 56, '#FF6F8E', 0.4));
}
// 16 cow
{
  const c = '#FFFFFF', k = '#35323A';
  add('Cow', body(c, `<ellipse cx="96" cy="250" rx="26" ry="18" fill="${k}"/><ellipse cx="172" cy="240" rx="18" ry="14" fill="${k}"/>`) +
      `<path d="M84 76 q-14 -20 -6 -40 q10 16 24 26z" fill="#EAD7AE"/><path d="M172 76 q14 -20 6 -40 q-10 16 -24 26z" fill="#EAD7AE"/>` +
      `<ellipse cx="50" cy="110" rx="28" ry="14" transform="rotate(20 50 110)" fill="${c}"/><ellipse cx="206" cy="110" rx="28" ry="14" transform="rotate(-20 206 110)" fill="${c}"/>` +
      `<ellipse cx="52" cy="111" rx="16" ry="7" transform="rotate(20 52 111)" fill="#F6B9C6"/><ellipse cx="204" cy="111" rx="16" ry="7" transform="rotate(-20 204 111)" fill="#F6B9C6"/>` +
      head(c) + clipped(`<ellipse cx="80" cy="84" rx="40" ry="32" fill="${k}"/><ellipse cx="190" cy="140" rx="26" ry="22" fill="${k}"/>`) +
      eyes(128, 118, 32, 8, INK) + `<ellipse cx="128" cy="164" rx="48" ry="32" fill="#F7BDC9"/><ellipse cx="112" cy="160" rx="6" ry="8" fill="#C86A82"/><ellipse cx="144" cy="160" rx="6" ry="8" fill="#C86A82"/>` +
      smile(120, 180, 8));
}
// 17 sheep
{
  const w = '#FFFFFF', f = '#F5DFCB';
  let wool = '';
  const puffs = [[128, 64, 30], [92, 72, 28], [164, 72, 28], [66, 100, 26], [190, 100, 26], [58, 136, 24], [198, 136, 24], [110, 56, 24], [146, 56, 24]];
  for (const [x, y, r] of puffs) wool += `<circle cx="${x}" cy="${y}" r="${r}" fill="${w}"/>`;
  let bodyWool = '';
  for (const [x, y] of [[60, 236], [100, 222], [156, 222], [196, 236], [128, 230]]) bodyWool += `<circle cx="${x}" cy="${y}" r="34" fill="${w}"/>`;
  add('Sheep', body(w, bodyWool) + wool +
      `<ellipse cx="62" cy="128" rx="24" ry="12" transform="rotate(25 62 128)" fill="#E9C7B0"/><ellipse cx="194" cy="128" rx="24" ry="12" transform="rotate(-25 194 128)" fill="#E9C7B0"/>` +
      `<ellipse cx="128" cy="146" rx="56" ry="62" fill="${f}"/>` +
      `<circle cx="128" cy="86" r="26" fill="${w}"/><circle cx="106" cy="94" r="20" fill="${w}"/><circle cx="150" cy="94" r="20" fill="${w}"/>` +
      eyes(128, 140, 24, 7.5) + `<ellipse cx="128" cy="166" rx="9" ry="6" fill="#D98A8A"/>` + catMouth(128, 174, 6) + blush(128, 168, 40));
}
// 18 tiger
{
  const c = '#F59B3A', k = '#3A2D2A', w = '#FFF6EA';
  add('Tiger', body(c, `<ellipse cx="128" cy="266" rx="46" ry="40" fill="${w}"/><path d="M42 240 l24 6 M214 240 l-24 6" stroke="${k}" stroke-width="8" stroke-linecap="round"/>`) +
      roundEars(c, '#FFE3C8', 62, 66, 24, 12) + `<circle cx="66" cy="66" r="24" fill="none" stroke="${k}" stroke-width="5"/><circle cx="190" cy="66" r="24" fill="none" stroke="${k}" stroke-width="5"/>` +
      head(c) + clipped(`<path d="M116 58 l12 22 l12 -22" fill="none" stroke="${k}" stroke-width="8" stroke-linejoin="round" stroke-linecap="round"/>` +
              `<path d="M44 112 l26 8 M44 134 l22 2 M212 112 l-26 8 M212 134 l-22 2" stroke="${k}" stroke-width="8" stroke-linecap="round"/>` +
              `<ellipse cx="96" cy="170" rx="34" ry="26" fill="${w}"/><ellipse cx="160" cy="170" rx="34" ry="26" fill="${w}"/>`) +
      gloss(128, 132, 80) + eyes(128, 124, 32) + triNose(128, 152, 8, '#E46E7E') + catMouth(128, 161, 8) + blush(128, 156, 54));
}
// 19 lion
{
  const m = '#C8692B', c = '#F5B652';
  let mane = '';
  for (let i = 0; i < 14; i++) {
    const a = i / 14 * Math.PI * 2;
    mane += `<circle cx="${(128 + Math.cos(a) * 82).toFixed(1)}" cy="${(132 + Math.sin(a) * 78).toFixed(1)}" r="30" fill="${m}"/>`;
  }
  add('Lion', body(c, `<ellipse cx="128" cy="266" rx="44" ry="38" fill="#FFF0D6"/>`) + mane + `<ellipse cx="128" cy="132" rx="86" ry="82" fill="${m}"/>` +
      roundEars(c, '#E89A4E', 64, 76, 18, 9) + head(c, 128, 138, 66, 62) + gloss(128, 138, 66) +
      `<ellipse cx="128" cy="166" rx="30" ry="22" fill="#FFF0D6"/>` + eyes(128, 130, 26, 8) +
      `<path d="M118 150 h20 q0 6 -10 12 q-10 -6 -10 -12z" fill="#6B3B2A" stroke="#6B3B2A" stroke-width="3" stroke-linejoin="round"/>` +
      catMouth(128, 167, 7) + blush(128, 160, 42));
}
// 20 monkey
{
  const c = '#8F5B3D', f = '#F4D0A6';
  add('Monkey', body(c) + `<circle cx="46" cy="132" r="26" fill="${c}"/><circle cx="46" cy="132" r="15" fill="${f}"/><circle cx="210" cy="132" r="26" fill="${c}"/><circle cx="210" cy="132" r="15" fill="${f}"/>` +
      head(c, 128, 130, 78, 76) + `<path d="M128 96 C96 70 58 100 76 138 C84 156 76 172 98 190 Q128 206 158 190 C180 172 172 156 180 138 C198 100 160 70 128 96Z" fill="${f}"/>` +
      gloss(128, 130, 78) + eyes(128, 126, 26, 8.5) + `<ellipse cx="122" cy="154" rx="3" ry="4" fill="${INK}"/><ellipse cx="134" cy="154" rx="3" ry="4" fill="${INK}"/>` +
      openSmile(128, 166, 14) + blush(128, 158, 48));
}
// 21 frog
{
  const c = '#6DC24B', w = '#E9F7C9';
  add('Frog', body(c, `<ellipse cx="128" cy="270" rx="54" ry="44" fill="${w}"/>`) +
      `<circle cx="84" cy="88" r="34" fill="${c}"/><circle cx="172" cy="88" r="34" fill="${c}"/>` +
      `<ellipse cx="128" cy="146" rx="94" ry="66" fill="${c}"/>` +
      `<circle cx="84" cy="86" r="22" fill="#FFFFFF"/><circle cx="172" cy="86" r="22" fill="#FFFFFF"/>` +
      eyes(128, 88, 44, 11) + gloss(128, 146, 94) +
      `<path d="M78 156 Q128 200 178 156" fill="none" stroke="${INK}" stroke-width="4.5" stroke-linecap="round"/>` +
      blush(128, 160, 64, '#FF7A93', 0.5, 15, 9) + `<circle cx="116" cy="136" r="3" fill="#3E8A2A"/><circle cx="140" cy="136" r="3" fill="#3E8A2A"/>`);
}
// 22 penguin
{
  const c = '#2F3B4E', w = '#FFFFFF';
  add('Penguin', body(c, `<ellipse cx="128" cy="276" rx="60" ry="56" fill="${w}"/><path d="M98 222 h60 l-6 12 h-48z" fill="#E8475A"/>`) +
      head(c, 128, 132, 80, 78) + `<path d="M128 108 C100 84 58 108 70 146 C78 176 100 198 128 198 C156 198 178 176 186 146 C198 108 156 84 128 108Z" fill="${w}"/>` +
      gloss(128, 132, 80) + eyes(128, 138, 28, 8.5) +
      `<path d="M114 156 h28 q-6 14 -14 16 q-8 -2 -14 -16z" fill="#F5A524" stroke="#F5A524" stroke-width="4" stroke-linejoin="round"/>` + blush(128, 162, 48));
}
// 23 owl
{
  const c = '#8E6B4F', f = '#F2DEC2';
  add('Owl', body(c, `<path d="M96 224 q8 10 16 0 q8 10 16 0 q8 10 16 0 q8 10 16 0" fill="none" stroke="#C9A37E" stroke-width="5" stroke-linecap="round"/><path d="M104 246 q8 10 16 0 q8 10 16 0 q8 10 16 0" fill="none" stroke="#C9A37E" stroke-width="5" stroke-linecap="round"/>`) +
      ear([66, 40], [62, 104], [106, 76], c) + ear([190, 40], [194, 104], [150, 76], c) +
      head(c, 128, 136, 84, 78) + gloss(128, 136, 84) +
      `<circle cx="94" cy="128" r="32" fill="${f}"/><circle cx="162" cy="128" r="32" fill="${f}"/>` +
      eyes(128, 128, 34, 13) + `<path d="M118 152 h20 l-10 18z" fill="#F2A53A" stroke="#F2A53A" stroke-width="5" stroke-linejoin="round"/>` + blush(128, 168, 56));
}
// 24 chick
{
  const c = '#FFD84E';
  add('Chick', body(c, `<ellipse cx="44" cy="244" rx="26" ry="16" transform="rotate(-30 44 244)" fill="#F5C232"/><ellipse cx="212" cy="244" rx="26" ry="16" transform="rotate(30 212 244)" fill="#F5C232"/>`) +
      `<path d="M128 74 q-10 -26 6 -38 q-2 16 12 22 q-4 -18 14 -20 q-10 14 -8 36z" fill="#F5C232"/>` +
      head(c, 128, 140, 82, 78) + gloss(128, 140, 82) + eyes(128, 136, 32, 9) +
      `<path d="M114 158 h28 q-6 14 -14 16 q-8 -2 -14 -16z" fill="#F58A2A" stroke="#F58A2A" stroke-width="4" stroke-linejoin="round"/>` + blush(128, 164, 54));
}
// 25 seal
{
  const c = '#B6C3D1', w = '#F4F7FA';
  add('Seal', body(c, `<ellipse cx="128" cy="270" rx="50" ry="40" fill="${w}"/>`) + head(c, 128, 136, 84, 78) + gloss(128, 136, 84) +
      eyes(128, 126, 36, 11) + `<ellipse cx="114" cy="164" rx="18" ry="14" fill="${w}"/><ellipse cx="142" cy="164" rx="18" ry="14" fill="${w}"/>` +
      `<ellipse cx="128" cy="152" rx="11" ry="8" fill="${INK}"/>` +
      `<circle cx="108" cy="162" r="2.4" fill="#8E9BAA"/><circle cx="114" cy="170" r="2.4" fill="#8E9BAA"/><circle cx="148" cy="162" r="2.4" fill="#8E9BAA"/><circle cx="142" cy="170" r="2.4" fill="#8E9BAA"/>` +
      blush(128, 160, 58));
}
// 26 hedgehog
{
  const s = '#8A6248', f = '#F4DCBF';
  let spikes = '';
  for (let i = 0; i <= 12; i++) {
    const a = Math.PI * (1.0 + i / 12), a0 = a - 0.14, a1 = a + 0.14;
    const p = (ang, r) => `${(128 + Math.cos(ang) * r).toFixed(1)} ${(140 + Math.sin(ang) * r).toFixed(1)}`;
    spikes += `<path d="M${p(a0 - 0.12, 70)} L${p(a, 112)} L${p(a1 + 0.12, 70)} Z" fill="${s}" stroke="${s}" stroke-width="6" stroke-linejoin="round"/>`;
  }
  add('Hedgehog', body(s, `<ellipse cx="128" cy="270" rx="50" ry="40" fill="${f}"/>`) + spikes + `<ellipse cx="128" cy="140" rx="92" ry="80" fill="${s}"/>` +
      `<path d="M128 104 C94 80 56 114 68 152 C78 186 104 202 128 202 C152 202 178 186 188 152 C200 114 162 80 128 104Z" fill="${f}"/>` +
      roundEars(f, '#E9B9A6', 54, 92, 13, 7) + eyes(128, 142, 28, 8.5) +
      `<ellipse cx="128" cy="164" rx="10" ry="7" fill="${INK}"/>` + smile(121, 174, 7) + blush(128, 170, 46));
}
// 27 octopus
{
  const c = '#F27A86';
  let arms = '';
  for (let i = 0; i < 5; i++) {
    const x = 60 + i * 34;
    arms += `<path d="M${x - 14} 190 Q${x - 18} 236 ${x} 256 Q${x + 18} 236 ${x + 14} 190 Z" fill="${shade(c, -0.08)}"/>`;
    arms += `<circle cx="${x}" cy="226" r="4" fill="#FFD0D6"/><circle cx="${x}" cy="242" r="3" fill="#FFD0D6"/>`;
  }
  add('Octopus', arms + `<ellipse cx="128" cy="124" rx="84" ry="86" fill="${c}"/>` + gloss(128, 124, 84) +
      `<circle cx="64" cy="98" r="8" fill="#FFD0D6" opacity=".8"/><circle cx="190" cy="84" r="6" fill="#FFD0D6" opacity=".8"/><circle cx="178" cy="62" r="4" fill="#FFD0D6" opacity=".8"/>` +
      eyes(128, 136, 32, 10) + smile(119, 162, 9) + blush(128, 160, 54, '#FF4F6E', 0.4));
}
// 28 ghost
{
  const c = '#FFFFFF';
  add('Ghost', `<path d="M48 136 C48 70 90 36 128 36 C166 36 208 70 208 136 L208 250 Q194 232 180 250 Q166 268 154 250 Q141 232 128 250 Q115 268 102 250 Q90 232 76 250 Q62 268 48 250 Z" fill="${c}"/>` +
      `<path d="M196 110 C204 140 206 180 204 236 Q194 232 186 240 C192 190 192 150 182 112Z" fill="#DDE6F4"/>` +
      `<ellipse cx="96" cy="130" rx="11" ry="15" fill="${INK}"/><ellipse cx="160" cy="130" rx="11" ry="15" fill="${INK}"/>` +
      `<circle cx="100" cy="124" r="4.5" fill="#FFF"/><circle cx="164" cy="124" r="4.5" fill="#FFF"/>` +
      `<ellipse cx="128" cy="168" rx="9" ry="11" fill="${INK}"/>` + blush(128, 156, 54, '#FF7A93', 0.5) +
      `<ellipse cx="44" cy="186" rx="16" ry="11" transform="rotate(-30 44 186)" fill="${c}"/><ellipse cx="212" cy="186" rx="16" ry="11" transform="rotate(30 212 186)" fill="#E9EFF8"/>`);
}
// 29 robot
{
  const c = '#C8D1DD', d = '#9AA6B6';
  add('Robot', body(d, `<rect x="88" y="214" width="80" height="60" rx="10" fill="${c}"/><circle cx="112" cy="236" r="6" fill="#E8475A"/><circle cx="132" cy="236" r="6" fill="#F7C948"/><circle cx="152" cy="236" r="6" fill="#4CC38A"/>`) +
      `<rect x="124" y="30" width="8" height="34" fill="${d}"/><circle cx="128" cy="30" r="12" fill="#F0544F"/><circle cx="124" cy="26" r="4" fill="#FFF" opacity=".6"/>` +
      `<rect x="34" y="108" width="20" height="48" rx="8" fill="${d}"/><rect x="202" y="108" width="20" height="48" rx="8" fill="${d}"/>` +
      `<rect x="48" y="60" width="160" height="148" rx="38" fill="${c}"/>` + `<rect x="66" y="82" width="124" height="96" rx="24" fill="#2F3644"/>` +
      `<path d="M88 126 q12 -16 24 0 M144 126 q12 -16 24 0" stroke="#5CE1E6" stroke-width="7" fill="none" stroke-linecap="round"/>` +
      `<path d="M110 148 q18 16 36 0" stroke="#5CE1E6" stroke-width="6" fill="none" stroke-linecap="round"/>` +
      blush(128, 150, 46, '#FF7A93', 0.55, 9, 5) + `<ellipse cx="88" cy="76" rx="22" ry="6" fill="#FFF" opacity=".35"/>`);
}
// 30 alien
{
  const c = '#8FDB6B';
  add('Alien', body('#7C69D9', `<circle cx="128" cy="240" r="10" fill="#F7C948"/>`) +
      `<path d="M104 64 Q96 44 84 36" stroke="${c}" stroke-width="7" fill="none" stroke-linecap="round"/><circle cx="82" cy="34" r="11" fill="#F7C948"/>` +
      `<path d="M152 64 Q160 44 172 36" stroke="${c}" stroke-width="7" fill="none" stroke-linecap="round"/><circle cx="174" cy="34" r="11" fill="#F7C948"/>` +
      `<path d="M128 56 C190 56 214 100 208 136 C202 176 164 214 128 214 C92 214 54 176 48 136 C42 100 66 56 128 56Z" fill="${c}"/>` + gloss(128, 128, 82) +
      `<ellipse cx="92" cy="132" rx="22" ry="30" transform="rotate(-24 92 132)" fill="${INK}"/><ellipse cx="164" cy="132" rx="22" ry="30" transform="rotate(24 164 132)" fill="${INK}"/>` +
      `<ellipse cx="98" cy="120" rx="7" ry="9" fill="#FFF"/><ellipse cx="170" cy="120" rx="7" ry="9" fill="#FFF"/>` +
      smile(120, 184, 8) + blush(128, 174, 52, '#FF7A93', 0.45));
}

module.exports = { characters, shade };
