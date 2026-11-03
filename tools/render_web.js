// Screenshots of the web dashboard with a mocked board API (no hardware needed).
// node tools/render_web.js  -> assets/screens/web_*.png
const { chromium } = require('playwright');
const path = require('path');
const ROOT = path.resolve(__dirname, '..');
const sp = (t, v, a) => ({ done: t > 0, t, v, a });
const best = [sp(3.41, 38.6, 205), sp(7.97, 90.4, 214), sp(11.30, 128.2, 190), sp(16.52, 140.0, 170)];
const live = (o = {}) => Object.assign({
  linked: true, speedOk: true, kmh: 62.4, rpm: 539, volt: 78.4, amp: 48.6, kw: 3.81, soc: 85, tc: 42, tm: 55, gear: 'H',
  status: 'RUN', faults: 0, fault: 'None', hz: 10.2, baud: 9600, src: 0,
  gps: { seen: true, fix: true, ubx: true, sats: 14, hz: 25, acc: 0.3 },
  trip: { km: 12.4, max: 98, avg: 37, maxA: 186, wh: 508, whkm: 41 }, odo: 1284, fw: 'VOLTOL Monitor 1.1.0', sd: false,
  freeKB: 512, maxKmh: 120, maxAmp: 200, dash: 0,
  drag: { state: 'IDLE', el: 0, dist: 0, spd: 0, gps: false, wait: false, roll: false, sp: [sp(0), sp(0), sp(0), sp(0)].map(x => ({ ...x, v: 0, a: 0 })), peakA: 0, peakKw: 0, spin: false, haveBest: true, best, bestWhen: '03 Nov 18:12' },
}, o);
const graph = { kmh: [], amp: [] };
for (let i = 0; i < 180; i++) { const t = i / 180; let k = 60 + 35 * Math.sin(t * 9) * (t < .5 ? t * 2 : 1) + (i > 150 ? -50 * (i - 150) / 30 : 0); graph.kmh.push(Math.max(0, k)); graph.amp.push(i > 150 ? -25 : 60 + 110 * Math.cos(t * 9 - .6) + (i % 7) * 4); }
const lists = {
  trips: [41, 40, 39, 38].map((id, i) => ({ id, when: ['03 Nov 17:40', '02 Nov 19:01', '02 Nov 08:12', '01 Nov 18:30'][i], km: [18.2, 22.7, 9.4, 31][i], max: 92 + i * 3, maxA: 170 + i * 6, whkm: 38 + i })),
  faults: [{ when: '18:31:07', set: true, name: 'Overcurrent', kmh: 64, amp: 205, volt: 74.2 }, { when: '18:31:09', set: false, name: 'None', kmh: 55, amp: 175, volt: 75.2 }, { when: '18:02:44', set: true, name: 'Hall fault', kmh: 46, amp: 145, volt: 76.2 }],
  runs: [{ when: '03 Nov 18:31', t: [3.33, 7.78, 11.04, 16.24], v: [39.6, 92.5, 131.2, 140], peakA: 210 }, { when: '03 Nov 18:12', t: [3.41, 7.97, 11.30, 16.52], v: [38.6, 90.4, 128.2, 140], peakA: 214 }],
  backups: [{ id: 4, when: '03 Nov 18:20', sum: 0xACB }, { id: 3, when: '01 Nov 09:12', sum: 0x1A684 }], graph,
};
const F = (i, g, n, u, d, v, o, st) => ({ i, g, n, u, d, v, o, r: Math.round(v * 10 ** d), ro: Math.round(o * 10 ** d), st, mn: 0, mx: 9999 });
const params = {
  have: true, busy: false, when: '18:20', sum: 0xACB, msg: 'Read OK', kind: 1, stopped: true, linked: true, lastBackup: 4, fields: [
    F(0, 'Battery', 'Overvoltage', 'V', 1, 90, 90, 1), F(1, 'Battery', 'Undervoltage', 'V', 1, 60, 60, 1), F(4, 'Battery', 'Busbar current', 'A', 0, 180, 180, 1), F(5, 'Battery', 'Phase current', 'A', 0, 450, 450, 1),
    F(6, 'Sport', 'Sport current limit', 'A', 0, 45, 30, 1), F(9, 'Sport', 'Speed ratio', '%', 0, 60, 40, 1), F(10, 'Sport', 'Soft-start grade', '', 0, 16, 16, 1)],
};
const settings = { dash: 0, speedSrc: 2, logRate: 0, cells: 20, cellEmptyMv: 3000, cellFullMv: 4150, maxKmh: 120, maxAmp: 200, backlight: 4, wifi: 1, tzHours: 7, rollout: 0, gpsRate: 1, tyreW: 130, tyreA: 70, tyreR: 17, wheelCorr: 0, ratioX100: 100, circMm: 1928, kmhAt1000: 115.7, headless: false };
async function shot(page, name, liveObj, setup) {
  await page.route('**/*', r => {
    const u = new URL(r.request().url());
    const json = b => r.fulfill({ contentType: 'application/json', body: JSON.stringify(b) });
    if (u.pathname === '/') return r.fulfill({ path: path.join(ROOT, 'assets/web/index.html') });
    if (u.pathname === '/api/live') return json(liveObj);
    if (u.pathname === '/api/lists') return json(lists);
    if (u.pathname === '/api/params') return json(params);
    if (u.pathname === '/api/settings') return json(settings);
    if (u.pathname === '/api/files') return json([{ n: '/log/T0042_detail.csv', s: 212300 }, { n: '/log/T0042_sec.csv', s: 31800 }, { n: '/faults.csv', s: 4200 }, { n: '/trips.csv', s: 900 }, { n: '/drag.csv', s: 320 }]);
    return json({});
  });
  await page.goto('http://voltol.local/');
  await page.waitForTimeout(400);
  if (setup) await setup(page);
  await page.waitForTimeout(500);
  await page.screenshot({ path: path.join(ROOT, 'assets/screens', name + '.png'), fullPage: true });
  await page.unrouteAll();
}
(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2 });
  const page = await ctx.newPage();
  const tab = t => async p => { await p.click(`nav button[data-t="${t}"]`); await p.waitForTimeout(400); };
  const style = s => async p => { await p.click(`#styleSeg button[data-s="${s}"]`); };
  await shot(page, 'web_01_dash_A', live(), style(0));
  await shot(page, 'web_02_dash_B', live({ kmh: 87, rpm: 752, amp: 132, kw: 10.1 }), style(1));
  await shot(page, 'web_03_dash_C', live({ kmh: 105, rpm: 908, amp: 172, kw: 13.3, gear: 'S' }), style(2));
  const run = live({ kmh: 83.6, amp: 186, drag: { ...live().drag, state: 'RUN', el: 7.42, dist: 142.6, spd: 83.6, peakA: 0, sp: [sp(3.33, 39.6, 205), sp(7.78, 92.5, 214), { done: false, t: 0, v: 0, a: 0 }, { done: false, t: 0, v: 0, a: 0 }] } });
  await shot(page, 'web_04_drag_run', run, async p => { await style(0)(p); await tab('drag')(p); });
  const gpsDone = live({ linked: false, kmh: 0, src: 1, drag: { ...live().drag, state: 'DONE', el: 15.81, dist: 402.3, gps: true, roll: true, sp: [sp(2.90, 39.6, 0), sp(7.36, 92.5, 0), sp(10.61, 131.2, 0), sp(15.81, 140.0, 0)] } });
  await shot(page, 'web_05_drag_gps_result', gpsDone, tab('drag'));
  await shot(page, 'web_06_log', live({ kmh: 0, rpm: 0, amp: 0, status: 'IDLE' }), tab('log'));
  await shot(page, 'web_07_tune', live({ kmh: 0, rpm: 0, amp: 0, status: 'IDLE' }), tab('tune'));
  await shot(page, 'web_08_tune_confirm', live({ kmh: 0, rpm: 0, amp: 0, status: 'IDLE' }), async p => { await tab('tune')(p); await p.click('#pWrite'); });
  await shot(page, 'web_09_setup', live(), tab('setup'));
  await b.close();
})();
