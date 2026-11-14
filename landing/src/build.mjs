// Builds the landing pages from src/page.html and src/strings.json:
//   page.uk.html, page.en.html  - the page per language (index.php serves them and fills <!--form-state-->)
//   strings.json                - the texts index.php and download.php use on the server
// Run from anywhere: node landing/src/build.mjs
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const src = dirname(fileURLToPath(import.meta.url));
const out = dirname(src);
const template = readFileSync(join(src, 'page.html'), 'utf8');
const strings = JSON.parse(readFileSync(join(src, 'strings.json'), 'utf8'));
const qr = JSON.parse(readFileSync(join(src, 'qr.json'), 'utf8').replace(/^﻿/, ''));

const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

// The side-view figure: one nested group per joint, so turning the shin at the ankle carries the knee, the thigh,
// the torso and the arm with it. Standing pose in a 160 x 220 box, facing right. A pose (degrees) bends it in place;
// without one, the hero's CSS loop bends it.
function figure(pose) {
  const rot = (deg, x, y) => (pose ? ` transform="rotate(${deg} ${x} ${y})"` : '');
  return `<svg class="sk" viewBox="0 0 160 220" aria-hidden="true" focusable="false">` +
    `<line x1="80" y1="205" x2="104" y2="205"/>` +
    `<g class="sk-shin"${rot(pose?.shin, 80, 205)}><line x1="80" y1="205" x2="80" y2="150"/>` +
      `<g class="sk-thigh"${rot(pose?.thigh, 80, 150)}><line x1="80" y1="150" x2="80" y2="95"/>` +
        `<g class="sk-torso"${rot(pose?.torso, 80, 95)}><line x1="80" y1="95" x2="80" y2="40"/>` +
          `<circle class="sk-head" cx="80" cy="22" r="13"/>` +
          `<g class="sk-arm"${rot(pose?.arm, 80, 40)}><line x1="80" y1="40" x2="80" y2="90"/><circle cx="80" cy="90" r="5"/></g>` +
          `<circle cx="80" cy="40" r="5"/>` +
        `</g><circle cx="80" cy="95" r="5"/>` +
      `</g><circle cx="80" cy="150" r="5"/>` +
    `</g><circle cx="80" cy="205" r="5"/><circle cx="104" cy="205" r="4"/></svg>`;
}

// The 17 MoveNet points (COCO order) found on the drawn boy, as fractions of the picture.
const KP_POLO = [[0.4737,0.1671],[0.497,0.127],[0.4271,0.1492],[0.572,0.1152],[0.4124,0.1661],[0.6688,0.2798],[0.3652,0.2581],[0.6788,0.4262],[0.3287,0.4037],[0.6269,0.5486],[0.3148,0.5164],[0.5724,0.5334],[0.4115,0.5247],[0.5684,0.7277],[0.3975,0.7235],[0.5974,0.9062],[0.38,0.906]];
const BONES = [[5,7],[7,9],[6,8],[8,10],[5,6],[5,11],[6,12],[11,12],[11,13],[13,15],[12,14],[14,16],[0,1],[0,2],[1,3],[2,4]];
function skeleton(kp) {
  const P = kp.map(([x, y]) => [Math.round(x * 1000), Math.round(y * 1500)]);
  const lines = BONES.map(([a, b]) => `<line x1="${P[a][0]}" y1="${P[a][1]}" x2="${P[b][0]}" y2="${P[b][1]}"/>`).join('');
  const dots = P.map(([x, y], i) => `<circle cx="${x}" cy="${y}" r="${i <= 4 ? 11 : 15}"/>`).join('');
  return `<svg viewBox="0 0 1000 1500" aria-hidden="true" focusable="false">${lines}${dots}</svg>`;
}

// The landing's own address as a QR code (made with the app's encoder, ExerciseUnlockCommon/QrCode.cpp).
function qrSvg(rows) {
  let d = '';
  rows.forEach((row, y) => [...row].forEach((c, x) => { if (c === '1') d += `M${x} ${y}h1v1h-1z`; }));
  return `<svg viewBox="0 0 ${rows.length} ${rows.length}" shape-rendering="crispEdges" aria-hidden="true" focusable="false"><path fill="#121633" d="${d}"/></svg>`;
}

// A short sound wave per cue: the ready cue rises, a rep is one blip, the error buzzes flat.
const wave = hs => `<span class="eu-wave">${hs.map(h => `<i style="height:${h}px"></i>`).join('')}</span>`;

const shared = {
  skeleton_squat: figure(null),
  skeleton_phone: figure({ shin: 28, thigh: -88, torso: 92, arm: -100 }),
  skeleton_polo: skeleton(KP_POLO),
  qr_svg: qrSvg(qr),
  wave_a: wave([4, 7, 10, 14, 18]),
  wave_b: wave([6, 16, 6]),
  wave_c: wave([12, 12, 12, 12, 12]),
};

for (const [lang, S] of Object.entries(strings)) {
  const faqItems = S.faq.map(([q, a]) => `        <div class="faq__item"><h3>${esc(q)}</h3><p>${esc(a)}</p></div>`).join('\n');
  const faqLd = JSON.stringify({
    '@context': 'https://schema.org', '@type': 'FAQPage',
    mainEntity: S.faq.map(([q, a]) => ({ '@type': 'Question', name: q, acceptedAnswer: { '@type': 'Answer', text: a } })),
  }).replace(/</g, '\\u003c');
  const vars = { ...S, ...shared, canonical_query: `?lang=${lang}`, faq_items: faqItems, faq_jsonld: faqLd };
  const missing = new Set();
  const html = template
    .replace(/\{\{\{(\w+)\}\}\}/g, (_, k) => (k in vars ? vars[k] : (missing.add(k), '')))
    .replace(/\{\{(\w+)\}\}/g, (_, k) => (k in vars ? esc(vars[k]) : (missing.add(k), '')));
  if (missing.size) throw new Error(`${lang}: no text for ${[...missing].join(', ')}`);
  writeFileSync(join(out, `page.${lang}.html`), html);
}

// What the server needs: every text but the FAQ (that lives in the pages).
const server = Object.fromEntries(Object.entries(strings).map(([lang, S]) => {
  const { faq, ...rest } = S;
  return [lang, rest];
}));
writeFileSync(join(out, 'strings.json'), JSON.stringify(server, null, 1));
console.log('built', Object.keys(strings).map(l => `page.${l}.html`).join(', '), '+ strings.json');
