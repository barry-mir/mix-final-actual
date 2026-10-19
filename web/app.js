"use strict";
// mix_final_ACTUAL.wav: the mixer page. Tracks (arrangement) and Mixer views, Inspector, Assistant.
// Contract with the server: docs/DAW_DESIGN.md.

// ---- basics -------------------------------------------------------------------------------------------------------
const T = new URLSearchParams(location.search).get("t") || "";
const api = (p, body) => fetch(`/api/${p}${p.includes("?") ? "&" : "?"}t=${T}`, body === undefined ? {} : { method: "POST", body: JSON.stringify(body) })
  .then(async r => {
    if (!r.ok) { let m = ""; try { m = (await r.json()).error || ""; } catch (_) {} const e = new Error(m || `${p.split("?")[0]}: ${r.status}`); e.status = r.status; throw e; }
    return r.json();
  });
const $ = id => document.getElementById(id);
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const IS_MAC = /Mac|iPhone|iPad/.test(navigator.platform);
const MOD = IS_MAC ? "Cmd" : "Ctrl";
const fmtTime = s => { s = Math.max(0, s || 0); return `${Math.floor(s / 60)}:${(s % 60).toFixed(1).padStart(4, "0")}`; };
const dbToLin = db => Math.pow(10, db / 20);
const GROUPS = ["Vocals", "Drums", "Bass", "Guitars", "Keys & Other"];
const GSHORT = ["VOX", "DRM", "BASS", "GTR", "KEYS"];
const GCOL = ["#e8c547", "#e5534b", "#6cb6ff", "#5fcf7a", "#c597f5"];
const gcol = g => g >= 0 && g < 5 ? GCOL[g] : "#6b7380";

function toast(text, err) {
  const e = $("toast");
  e.textContent = text; e.className = err ? "err" : ""; e.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => { e.hidden = true; }, 3500);
}
function showErr(e) {
  const box = el("div"); box.id = "err";
  box.textContent = e && (e.status === 401 || e.status === 403)
    ? "This link is out of date: the mixer was restarted, and each start makes a new link. Use the link your coding agent (or the terminal) gave for this session."
    : `Lost the mixer (${e && e.message}). Is mix-final-actual still running?`;
  $("center").replaceChildren(box);
  stopped = true;
}
let stopped = false;

// ---- state --------------------------------------------------------------------------------------------------------
let S = null;          // session: tracks, params, rate, peaks_block
let V = {};            // param values by index
const byKey = {};      // "t|module.name" -> spec
const byId = {};       // "t1.eq.lf_gain" -> spec
let ST = {};           // last status
let E = [];            // clips per track: [{pos, src, len, gain, fadeIn, fadeOut}]
const PK = [];         // peaks per track (Uint8Array)
let sel = { track: null, clip: null }; // track index (-1 master), clip index on that track
let view = "tracks";
const P = (t, k) => byKey[`${t}|${k}`];
const pv = (t, k) => { const s = P(t, k); return s ? V[s.i] : undefined; };
const pon = (t, k) => pv(t, k) > 0.5;
const nTracks = () => (S ? S.tracks.length : 0);
const songLen = () => Math.max(ST.song_len || 0, ST.len || 0, 1);
const trackLen = t => (S && S.tracks[t] && S.tracks[t].len) || ST.len || songLen();

// ---- value mapping and formatting ------------------------------------------------------------------------------
const isTaper = s => s.minusInf && s.unit === "dB";
const isLog = s => s.min > 0 && s.max / s.min >= 20 && ["Hz", "ms", ":1", "s"].includes(s.unit);
function norm(s, v) {
  if (isTaper(s)) return v <= s.min + 1e-3 ? 0 : clamp(1 - Math.sqrt(Math.max(0, (s.max - v) / (s.max + 60))), 0, 1);
  if (isLog(s)) return clamp(Math.log(v / s.min) / Math.log(s.max / s.min), 0, 1);
  return clamp((v - s.min) / (s.max - s.min), 0, 1);
}
function fromNorm(s, n) {
  let v;
  if (isTaper(s)) v = n <= 0.002 ? s.min : Math.max(s.min, s.max - (1 - n) ** 2 * (s.max + 60));
  else if (isLog(s)) v = s.min * (s.max / s.min) ** n;
  else v = s.min + (s.max - s.min) * n;
  const st = s.step > 0 ? s.step : 0;
  if (st >= 0.01) v = Math.round(v / st) * st;
  return clamp(v, s.min, s.max);
}
function fmt(s, v) {
  if (v === undefined) return "";
  if (s.kind === "bool") return v > 0.5 ? "on" : "off";
  if (s.kind === "choice") return (s.choices[Math.round(v)] || "").replace("_", " > ") + (s.unit ? " " + s.unit : "");
  if (s.minusInf && v <= s.min + 1e-3) return "-inf";
  if (s.unit === "Hz" && v >= 1000) return `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz`;
  const d = s.unit === "Hz" || s.unit === "ms" && v >= 100 ? 0 : s.step >= 1 ? 0 : Math.abs(v) < 10 ? 2 : 1;
  const u = s.unit === ":1" ? ":1" : s.unit ? " " + s.unit : "";
  return v.toFixed(d) + u;
}
const fmtDb = (v, minus) => v <= -89.9 && minus ? "-inf" : (v > 0.04 ? "+" : "") + v.toFixed(1);
const panTxt = b => Math.abs(b) < 0.005 ? "C" : (b < 0 ? "L" : "R") + Math.round(Math.abs(b) * 100);
const hzTxt = f => f >= 1000 ? `${(f / 1000).toFixed(f >= 10000 ? 0 : 1)}k` : `${Math.round(f)}`;

// ---- parameters, bindings, undo ---------------------------------------------------------------------------------
const controls = {};
function bind(i, fn) { (controls[i] ||= []).push(fn); fn(V[i]); }
function refresh(i) { (controls[i] || []).forEach(f => f(V[i])); markDirty(i); }
const pendingPost = new Map();
function flushPosts() {
  for (const [i, v] of pendingPost) api("param", { i, v }).catch(showErr);
  pendingPost.clear();
}
const localSetAt = new Map(); // param -> when the page last set it
function setParam(i, v, { record = true } = {}) {
  const from = V[i];
  if (from === v) return;
  V[i] = v;
  localSetAt.set(i, Date.now());
  refresh(i);
  if (!pendingPost.size) requestAnimationFrame(flushPosts);
  pendingPost.set(i, v);
  if (record) pushUndo({ kind: "params", list: [{ i, from, to: v }], label: specLabel(i) });
}
const specById = i => S.params.find(p => p.i === i);
function specLabel(i) {
  const s = S && (S.paramsByI || {})[i];
  if (!s) return "change";
  const who = s.track >= 0 ? S.tracks[s.track].name : "Master";
  return `${who} ${s.module.replace(/_/g, " ")} ${s.name.replace(/_/g, " ")}`;
}
const UNDO = [], REDO = [];
function pushUndo(a) {
  const last = UNDO[UNDO.length - 1];
  // Wheel nudges and repeated small moves on the same control merge into one step.
  if (a.merge && last && last.merge === a.merge && Date.now() - last.time < 800) { last.list[0].to = a.list[0].to; last.time = Date.now(); return; }
  a.time = Date.now();
  UNDO.push(a); if (UNDO.length > 400) UNDO.shift();
  REDO.length = 0;
}
function applyAction(a, dir) {
  if (a.kind === "params") for (const c of (dir === "undo" ? [...a.list].reverse() : a.list)) setParam(c.i, dir === "undo" ? c.from : c.to, { record: false });
  else if (a.kind === "clips") postClips(a.track, dir === "undo" ? a.before : a.after, { record: false });
}
function undo() { const a = UNDO.pop(); if (!a) return toast("Nothing to undo"); applyAction(a, "undo"); REDO.push(a); toast(`Undo: ${a.label}`); }
function redo() { const a = REDO.pop(); if (!a) return toast("Nothing to redo"); applyAction(a, "redo"); UNDO.push(a); toast(`Redo: ${a.label}`); }

// ---- controls -------------------------------------------------------------------------------------------------------
const tip = $("valTip");
function showTip(e, text) { tip.textContent = text; tip.hidden = false; tip.style.left = `${e.clientX + 14}px`; tip.style.top = `${e.clientY - 26}px`; }
// A DAW-style control: drag (relative), Shift/Cmd/Ctrl-drag fine, wheel nudges, double-click or Alt-click resets.
// spec: a server parameter, or {min, max, def, unit, step, get(), set(v, final)} for non-parameter values (clips).
function slider(s, { vertical = false, cls = "" } = {}) {
  const e = el("div", `ctl ${vertical ? "v" : "h"} ${cls}`);
  const bip = s.min < 0 && s.max > 0 && !isTaper(s);
  if (bip) e.classList.add("bip");
  const fill = el("i", "fill"), th = el("i", "thumb");
  if (!vertical) e.append(fill);
  e.append(th);
  if (vertical && isTaper(s)) for (const db of [6, 0, -6, -12, -24, -40]) if (db <= s.max) { const k = el("i", "tick"); k.style.top = `calc(4px + ${(1 - norm(s, db)) * 100}% - ${(1 - norm(s, db)) * 8}px)`; e.append(k); }
  const custom = typeof s.get === "function";
  const get = () => custom ? s.get() : V[s.i];
  const put = (v, final) => custom ? s.set(v, final) : setParam(s.i, v, { record: false });
  const paint = v => {
    const n = norm(s, v ?? s.def);
    if (vertical) th.style.top = `calc(6px + ${(1 - n) * 100}% - ${(1 - n) * 12}px)`;
    else {
      th.style.left = `calc(5px + ${n * 100}% - ${n * 10}px)`;
      const z = bip ? norm(s, 0) : 0;
      fill.style.left = `${Math.min(z, n) * 100}%`; fill.style.width = `${Math.abs(n - z) * 100}%`;
    }
    e.title = fmt(s, v);
  };
  e.paint = paint;
  if (!custom) bind(s.i, paint); else paint(get());
  const commit = (from, to, merge) => {
    if (from === to) return;
    if (custom) s.set(to, true);
    else pushUndo({ kind: "params", list: [{ i: s.i, from, to }], label: specLabel(s.i), merge });
  };
  e.onpointerdown = ev => {
    if (ev.button !== 0) return;
    ev.preventDefault(); ev.stopPropagation();
    const from = get();
    if (ev.altKey) { put(s.def, true); if (!custom) commit(from, s.def); if (custom) paint(s.def); return; }
    e.setPointerCapture(ev.pointerId);
    const n0 = norm(s, from), p0 = vertical ? ev.clientY : ev.clientX;
    const len = Math.max(40, (vertical ? e.clientHeight : e.clientWidth) - 10);
    let last = from;
    e.onpointermove = m => {
      const fine = m.shiftKey || m.metaKey || m.ctrlKey ? 0.1 : 1;
      const d = (vertical ? p0 - m.clientY : m.clientX - p0) / len * fine;
      last = fromNorm(s, clamp(n0 + d, 0, 1));
      put(last, false); if (custom) paint(last);
      showTip(m, fmt(s, last));
    };
    e.onpointerup = () => { e.onpointermove = null; tip.hidden = true; commit(from, last); };
  };
  e.ondblclick = ev => { ev.stopPropagation(); const from = get(); put(s.def, true); if (custom) paint(s.def); else commit(from, s.def); };
  e.onwheel = ev => {
    ev.preventDefault();
    const from = get();
    const step = (ev.shiftKey ? 0.002 : 0.01) * (ev.deltaY > 0 || ev.deltaX < 0 ? -1 : 1);
    const v = fromNorm(s, clamp(norm(s, from) + step, 0, 1));
    put(v, true); if (custom) paint(v); else commit(from, v, `w${s.i}`);
    showTip(ev, fmt(s, v)); clearTimeout(e.tt); e.tt = setTimeout(() => { tip.hidden = true; }, 700);
  };
  return e;
}
function toggle(s, label, cls = "") {
  const b = el("button", cls, label);
  b.onclick = ev => { ev.stopPropagation(); setParam(s.i, V[s.i] > 0.5 ? 0 : 1); };
  bind(s.i, v => { b.classList.toggle("on", v > 0.5); if (!label) b.textContent = v > 0.5 ? "on" : "off"; });
  return b;
}
function choice(s) {
  const c = el("select");
  s.choices.forEach((x, k) => c.add(new Option(x.replace("_", " > ") + (s.unit ? " " + s.unit : ""), k)));
  c.onchange = () => setParam(s.i, +c.value);
  bind(s.i, v => { c.value = Math.round(v); });
  return c;
}
function paramRow(s, label) {
  const r = el("div", "prow");
  r.append(el("label", "", label || s.name.replace(/_/g, " ")));
  const out = el("output");
  if (s.kind === "bool") { r.append(toggle(s, "", "bt"), out); }
  else if (s.kind === "choice") { r.append(choice(s), out); }
  else { r.append(slider(s)); bind(s.i, v => { out.textContent = fmt(s, v); }); r.append(out); }
  return r;
}

// ---- EQ / filter response ------------------------------------------------------------------------------------------
const FS = 48000;
function biquad(type, f0, g, q) {
  const A = Math.pow(10, g / 40), w = 2 * Math.PI * f0 / FS, cs = Math.cos(w), sn = Math.sin(w);
  if (type === "peak") { const al = sn / (2 * q); return [1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A]; }
  const al = sn / 2 * Math.sqrt((A + 1 / A) * (1 / 1 - 1) + 2), sq = 2 * Math.sqrt(A) * al;
  if (type === "ls") return [A * ((A + 1) - (A - 1) * cs + sq), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sq), (A + 1) + (A - 1) * cs + sq, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sq];
  return [A * ((A + 1) + (A - 1) * cs + sq), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sq), (A + 1) - (A - 1) * cs + sq, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sq];
}
function bqMag(c, f) {
  const w = 2 * Math.PI * f / FS, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  const nr = c[0] + c[1] * c1 + c[2] * c2, ni = -(c[1] * s1 + c[2] * s2), dr = c[3] + c[4] * c1 + c[5] * c2, di = -(c[4] * s1 + c[5] * s2);
  return Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}
const BANDS = ["lf", "lmf", "hmf", "hf"];
const BCOL = ["#f0a640", "#e8c547", "#6cb6ff", "#c597f5"];
function eqSections(t, only = -1) {
  const secs = [];
  if (only < 0) {
    if (pon(t, "hpf.on")) secs.push({ hp: pv(t, "hpf.freq"), n: [2, 3, 4][Math.round(pv(t, "hpf.slope"))] });
    if (pon(t, "lpf.on")) secs.push({ lp: pv(t, "lpf.freq"), n: Math.round(pv(t, "lpf.slope")) ? 4 : 2 });
  }
  if (pon(t, "eq.on"))
    BANDS.forEach((b, k) => {
      if ((only >= 0 && only !== k) || !pon(t, `eq.${b}_on`)) return;
      const f = pv(t, `eq.${b}_freq`), g = pv(t, `eq.${b}_gain`), q = pv(t, `eq.${b}_q`);
      const shelf = (k === 0 || k === 3) && Math.round(pv(t, `eq.${b}_shape`)) === 0;
      secs.push({ c: biquad(shelf ? (k === 0 ? "ls" : "hs") : "peak", f, g, q) });
    });
  return secs;
}
function respDb(secs, f) {
  let m = 1;
  for (const s of secs) {
    if (s.c) m *= bqMag(s.c, f);
    else if (s.hp) m *= 1 / Math.sqrt(1 + Math.pow(s.hp / f, 2 * s.n));
    else m *= 1 / Math.sqrt(1 + Math.pow(f / s.lp, 2 * s.n));
  }
  return 20 * Math.log10(Math.max(m, 1e-9));
}
function sizeCanvas(c) {
  const r = window.devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
  if (c.width !== Math.round(w * r) || c.height !== Math.round(h * r)) { c.width = Math.round(w * r); c.height = Math.round(h * r); }
  const g = c.getContext("2d"); g.setTransform(r, 0, 0, r, 0, 0);
  return [g, w, h];
}
const fToX = (f, w) => Math.log(f / 20) / Math.log(1000) * w;
const xToF = (x, w) => 20 * Math.pow(1000, x / w);
function drawCurve(c, t, { big = false } = {}) {
  const [g, w, h] = sizeCanvas(c);
  g.clearRect(0, 0, w, h);
  const range = big ? 18 : 24, yOf = db => h / 2 - db / range * (h / 2 - 3);
  g.strokeStyle = "#2a2f36"; g.lineWidth = 1;
  for (const f of [100, 1000, 10000]) { const x = Math.round(fToX(f, w)) + .5; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke(); }
  if (big) {
    g.fillStyle = "#5d6570"; g.font = "9px sans-serif";
    for (const [f, l] of [[100, "100"], [1000, "1k"], [10000, "10k"]]) g.fillText(l, fToX(f, w) + 2, h - 3);
    for (const db of [-12, -6, 6, 12]) { const y = Math.round(yOf(db)) + .5; g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke(); g.fillText(`${db > 0 ? "+" : ""}${db}`, 2, y - 2); }
  }
  if (big && SPEC.track === t && SPEC.post && SPEC.hz) drawSpectrum(g, w, h);
  if (t < 0) return; // master: the spectrum only
  g.strokeStyle = "#3a4048"; g.beginPath(); g.moveTo(0, h / 2 + .5); g.lineTo(w, h / 2 + .5); g.stroke();
  const secs = eqSections(t), eqOn = pon(t, "eq.on");
  g.beginPath();
  for (let x = 0; x <= w; x += 1) { const y = clamp(yOf(respDb(secs, xToF(x, w))), 1, h - 1); x ? g.lineTo(x, y) : g.moveTo(x, y); }
  g.strokeStyle = eqOn || secs.length ? "#f0a640" : "#5d6570"; g.lineWidth = big ? 2 : 1.4; g.stroke();
  g.lineTo(w, h / 2); g.lineTo(0, h / 2); g.closePath(); g.fillStyle = "#f0a64018"; g.fill();
  if (big && eqOn) BANDS.forEach((b, k) => {
    if (!pon(t, `eq.${b}_on`)) return;
    const x = fToX(pv(t, `eq.${b}_freq`), w), y = yOf(pv(t, `eq.${b}_gain`));
    g.beginPath(); g.arc(x, y, 6, 0, 7); g.fillStyle = BCOL[k]; g.fill(); g.strokeStyle = "#111"; g.lineWidth = 1.5; g.stroke();
    g.fillStyle = "#111"; g.font = "bold 8px sans-serif"; g.fillText(k + 1, x - 2.5, y + 3);
  });
}
// Live spectrum of the selected track (input as a line, output filled), drawn behind its EQ curve while playing.
let SPEC = { track: null };
function setScope(t) {
  if (SPEC.track === t) return;
  SPEC = { track: t };
  api("scope", { track: t }).catch(() => {});
}
function drawSpectrum(g, w, h) {
  // The scale follows the signal: its top sits 6 dB over the loudest band (falling slowly), 66 dB tall.
  const peak = Math.max(...SPEC.post, ...(SPEC.pre || [-200]));
  const want = Math.max(-80, peak + 6);
  SPEC.top = SPEC.top === undefined || want > SPEC.top ? want : SPEC.top - Math.min(0.4, SPEC.top - want);
  const yOf = db => h - clamp((db - (SPEC.top - 66)) / 66, 0, 1) * h;
  const path = arr => { g.beginPath(); arr.forEach((db, k) => { const x = fToX(SPEC.hz[k], w), y = yOf(db); k ? g.lineTo(x, y) : g.moveTo(x, y); }); };
  if (SPEC.pre) { path(SPEC.pre); g.strokeStyle = "#8d95a066"; g.lineWidth = 1; g.stroke(); }
  path(SPEC.post); g.lineTo(w, h); g.lineTo(0, h); g.closePath(); g.fillStyle = "#6cb6ff26"; g.fill();
  path(SPEC.post); g.strokeStyle = "#6cb6ffaa"; g.lineWidth = 1; g.stroke();
}
async function pollSpectrum() {
  if (stopped) return;
  const cs = document.querySelectorAll("#inspector canvas.eqbig");
  if (cs.length && SPEC.track !== null && (ST.playing || SPEC.post)) {
    try {
      const r = await api("spectrum");
      if (r && r.track === SPEC.track && r.post) {
        SPEC.hz = r.hz || SPEC.hz; SPEC.pre = r.pre; SPEC.post = r.post;
        if (!ST.playing) SPEC.post = SPEC.pre = null; // stopped: clear once
        cs.forEach(c => drawCurve(c, +c.dataset.curve, { big: true }));
      }
    } catch (_) {}
  }
  setTimeout(pollSpectrum, 70);
}
// Redraw curves after EQ/filter parameters change (batched per frame).
const curveDirty = new Set();
function markDirty(i) {
  const s = S && S.paramsByI && S.paramsByI[i];
  if (!s) return;
  if (["eq", "hpf", "lpf"].includes(s.module)) curveDirty.add(s.track);
  blocksDirty.add(s.track);
  if (!markDirty.raf) markDirty.raf = requestAnimationFrame(() => {
    markDirty.raf = 0;
    for (const t of curveDirty) document.querySelectorAll(`canvas[data-curve="${t}"]`).forEach(c => drawCurve(c, t, { big: c.classList.contains("eqbig") }));
    curveDirty.clear();
    for (const t of blocksDirty) { updateBlocks(t); updateDots(t); }
    blocksDirty.clear();
  });
}
const blocksDirty = new Set();

// ---- per-track summaries (as on the plugin's strips) ------------------------------------------------------------
function trackBlocks(t) {
  const L = [];
  const add = (id, label, color, on, text, toggleKey) => L.push({ id, label, color, on, text, toggleKey });
  const trim = pv(t, "trim.gain"), pl = pon(t, "polarity.inv_l"), pr = pon(t, "polarity.inv_r");
  add("input", "in", "#8d95a0", Math.abs(trim) > 0.05 || pl || pr, `trim ${fmtDb(trim)}${pl || pr ? " · Ø" + (pl ? "L" : "") + (pr ? "R" : "") : ""}`);
  const hp = pon(t, "hpf.on"), lp = pon(t, "lpf.on");
  add("filters", "filt", "var(--filter)", hp || lp, hp || lp ? [hp ? `HP ${hzTxt(pv(t, "hpf.freq"))}` : "", lp ? `LP ${hzTxt(pv(t, "lpf.freq"))}` : ""].filter(Boolean).join(" · ") : "off");
  add("gate", "gate", "var(--gate)", pon(t, "gate.on"), pon(t, "gate.on") ? `${pv(t, "gate.threshold").toFixed(0)} dB · ${pv(t, "gate.range").toFixed(0)}` : "off", "gate.on");
  const parts = [];
  BANDS.forEach(b => { const g = pv(t, `eq.${b}_gain`); if (pon(t, `eq.${b}_on`) && Math.abs(g) >= 0.05) parts.push(`${b.toUpperCase()} ${fmtDb(g)}`); });
  add("eq", "eq", "var(--eq)", pon(t, "eq.on") && parts.length > 0, pon(t, "eq.on") ? parts.join(" · ") || "flat" : "off", "eq.on");
  add("comp", "comp", "var(--comp)", pon(t, "comp.on"), pon(t, "comp.on") ? `${pv(t, "comp.ratio").toFixed(1)}:1 · ${pv(t, "comp.threshold").toFixed(0)} dB` : "off", "comp.on");
  add("deesser", "de-s", "var(--comp)", pon(t, "deesser.on"), pon(t, "deesser.on") ? `${pv(t, "deesser.threshold").toFixed(0)} dB · ${hzTxt(pv(t, "deesser.freq"))}` : "off", "deesser.on");
  const dr = pv(t, "drive.amount");
  add("drive", "drive", "var(--drive)", dr >= 0.05, dr >= 0.05 ? `${dr.toFixed(1)} dB` : "off");
  add("doubler", "dbl", "var(--filter)", pon(t, "doubler.on"), pon(t, "doubler.on") ? `mix ${pv(t, "doubler.mix").toFixed(2)} · ${pv(t, "doubler.delay").toFixed(0)} ms` : "off", "doubler.on");
  const w = pv(t, "width.amount");
  add("pan", "pan", "var(--accent)", true, `${panTxt(pv(t, "balance.value"))} · W ${w.toFixed(2)}`);
  const sg = pv(t, "send.gain"), eg = pv(t, "echo_send.gain");
  add("sends", "send", "var(--send)", sg > -89.9 || eg > -89.9, `rev ${fmtDb(sg, true)} · echo ${fmtDb(eg, true)}`);
  const grp = S.tracks[t].group;
  add("bus", "bus", gcol(grp), grp >= 0, grp >= 0 ? `→ ${GROUPS[grp]} bus` : "→ mix");
  return L;
}
function masterBlocks() {
  const t = -1, L = [];
  const add = (id, label, color, on, text, toggleKey) => L.push({ id, label, color, on, text, toggleKey });
  add("buscomp", "glue", "var(--comp)", pon(t, "buscomp.on"), pon(t, "buscomp.on") ? `${pv(t, "buscomp.ratio").toFixed(1)}:1 · ${pv(t, "buscomp.threshold").toFixed(0)} dB` : "off", "buscomp.on");
  add("limiter", "lim", "var(--hot)", pon(t, "limiter.on"), pon(t, "limiter.on") ? `${pv(t, "limiter.ceiling").toFixed(1)} dBFS` : "off", "limiter.on");
  const L2 = ST.loudness || {};
  add("loud", "loud", "var(--meter)", L2.target != null, L2.target != null ? `${L2.target} LUFS · ${fmtDb(L2.gain_db || 0)} dB` : "as mixed");
  return L;
}
function blockRows(t) {
  const box = el("div", "blks");
  box.dataset.blocks = t;
  fillBlocks(box, t);
  return box;
}
function fillBlocks(box, t) {
  box.replaceChildren();
  for (const b of (t < 0 ? masterBlocks() : trackBlocks(t))) {
    const r = el("div", "blk" + (b.on ? "" : " off"));
    const k = el("i", "k"); k.style.background = b.color;
    r.append(k, el("span", "l", b.label), el("span", "v", b.text));
    r.title = `${b.text}. Click: open in the inspector${b.toggleKey ? ". Alt-click: turn on/off" : ""}`;
    r.onclick = ev => {
      ev.stopPropagation();
      if (ev.altKey && b.toggleKey) { const s = P(t, b.toggleKey); setParam(s.i, V[s.i] > 0.5 ? 0 : 1); return; }
      selectTrack(t, { section: b.id });
    };
    box.append(r);
  }
}
function updateBlocks(t) { document.querySelectorAll(`[data-blocks="${t}"]`).forEach(b => fillBlocks(b, t)); }

// ---- selection --------------------------------------------------------------------------------------------------------
function selectTrack(t, { section = null, clip = null, keepClip = false } = {}) {
  const changed = sel.track !== t;
  sel.track = t;
  if (t !== null) setScope(t < -1 ? null : t); // spectrum: tracks and the master
  if (!keepClip) sel.clip = clip;
  document.querySelectorAll(".th").forEach(h => h.classList.toggle("sel", +h.dataset.t === t));
  document.querySelectorAll(".strip").forEach(h => h.classList.toggle("sel", +h.dataset.t === t));
  if (changed || section || !keepClip) buildInspector();
  if (section) {
    const s = document.querySelector(`#inspector [data-sec="${section}"]`);
    if (s) { $("inspector").hidden = false; s.scrollIntoView({ block: "nearest" }); s.classList.remove("flash"); void s.offsetWidth; s.classList.add("flash"); }
  }
  drawLanes();
  if (t !== null && t >= 0) ensureTrackVisible(t);
}

// ---- inspector -----------------------------------------------------------------------------------------------------
const SECTIONS = [
  ["input", "input", ["trim", "polarity"], null],
  ["filters", "filters", ["hpf", "lpf"], null],
  ["gate", "gate", ["gate"], "gate.on"],
  ["eq", "eq", ["eq"], "eq.on"],
  ["comp", "compressor", ["comp", "strip"], "comp.on"],
  ["deesser", "de-esser", ["deesser"], "deesser.on"],
  ["drive", "drive", ["drive"], null],
  ["doubler", "doubler", ["doubler"], "doubler.on"],
  ["pan", "pan & width", ["balance", "width"], null],
  ["sends", "sends", ["send", "echo_send"], null],
  ["output", "output", ["fader", "mute", "solo"], null],
];
const MSECTIONS = [
  ["reverb", "reverb", ["reverb"], null], ["echo", "echo", ["echo"], null],
  ["bus_vocals", "vocal bus", ["bus_vocals"], "bus_vocals.on"], ["bus_drums", "drum bus", ["bus_drums"], "bus_drums.on"],
  ["bus_bass", "bass bus", ["bus_bass"], "bus_bass.on"], ["bus_guitars", "guitar bus", ["bus_guitars"], "bus_guitars.on"],
  ["bus_keys", "keys & other bus", ["bus_keys"], "bus_keys.on"],
  ["buscomp", "glue compressor", ["buscomp"], "buscomp.on"], ["limiter", "master fader & limiter", ["fader", "limiter"], "limiter.on"],
];
const ROWLABEL = { "hpf.on": "HPF", "hpf.freq": "HPF freq", "hpf.slope": "HPF slope", "lpf.on": "LPF", "lpf.freq": "LPF freq", "lpf.slope": "LPF slope",
  "polarity.inv_l": "invert L", "polarity.inv_r": "invert R", "strip.order": "order", "balance.value": "pan", "width.amount": "width",
  "send.gain": "reverb send", "send.pre": "reverb pre", "echo_send.gain": "echo send", "fader.gain": "fader", "trim.gain": "trim", "drive.amount": "drive",
  "mute.on": "mute", "solo.on": "solo" };
function buildInspector() {
  const box = $("inspector");
  // Drop the bindings of the controls being replaced (they hold detached elements).
  for (const k of Object.keys(controls)) controls[k] = controls[k].filter(f => !f.insp);
  box.replaceChildren();
  const t = sel.track;
  if (t === null || !S) { box.append(el("div", "empty", "Select a track to see its controls.")); return; }
  const h = el("h2");
  if (t >= 0) {
    const chip = el("span", "chip", t + 1); chip.style.background = gcol(S.tracks[t].group);
    h.append(chip, el("span", "", S.tracks[t].name));
  } else {
    const g = -2 - t;
    const chip = el("span", "chip", t === -1 ? "M" : t === REV_ID ? "REV" : t === ECHO_ID ? "ECHO" : GSHORT[g]);
    chip.style.background = t === -1 ? "#e6e8eb" : t <= REV_ID ? "#4fd1c5" : GCOL[g];
    h.append(chip, el("span", "", t === -1 ? "Master" : t === REV_ID ? "Reverb return" : t === ECHO_ID ? "Echo return" : `${GROUPS[g]} bus`));
  }
  box.append(h);
  if (t < -1 && t > REV_ID) { // a group bus: which tracks feed it
    const g = -2 - t, names = S.tracks.map((tr, k) => tr.group === g ? `${k + 1} ${tr.name}` : null).filter(Boolean);
    const m = el("div", "sec"); m.append(el("h3", "", "tracks on this bus"), el("div", "empty", names.length ? names.join(", ") : "none (set a track's group in its inspector)"));
    box.append(m);
  }
  if (t === -1) {
    const sp = el("div", "sec"); sp.dataset.sec = "spectrum";
    sp.append(el("h3", "", "spectrum: before the master chain (line), output (filled)"));
    const c = el("canvas", "eqbig"); c.dataset.curve = -1; sp.append(c);
    requestAnimationFrame(() => drawCurve(c, -1, { big: true }));
    box.append(sp);
  }
  if (t >= 0) {
    const g = el("div", "sec"); g.dataset.sec = "group";
    g.append(el("h3", "", "instrument group"));
    const r = el("div", "prow wide"); r.append(el("label", "", "group"));
    const s = el("select");
    s.add(new Option(`by name (${S.tracks[t].group >= 0 && !S.tracks[t].group_set ? GROUPS[S.tracks[t].group] : "auto"})`, "auto"));
    GROUPS.forEach((x, k) => s.add(new Option(x, k)));
    s.add(new Option("none", -1));
    s.value = S.tracks[t].group_set ? S.tracks[t].group : "auto";
    s.onchange = async () => { await api("track", { index: t, group: s.value === "auto" ? null : +s.value }).catch(e => toast(e.message, true)); reloadSession(); };
    r.append(s); g.append(r); box.append(g);
    box.append(clipSection(t));
  }
  const pt = t < 0 ? -1 : t; // buses, returns and the master are all master-level parameters
  const mods = {};
  for (const s of S.params) if (s.track === pt) (mods[s.module] ||= []).push(s);
  const only = t >= 0 ? null : t === -1 ? ["buscomp", "limiter"] : t === REV_ID ? ["reverb"] : t === ECHO_ID ? ["echo"] : [`bus_${BUSKEYS[-2 - t]}`];
  for (const [id, title, modules, powerKey] of t >= 0 ? SECTIONS : MSECTIONS) {
    if (only && !only.includes(id)) continue;
    const sec = el("div", "sec"); sec.dataset.sec = id;
    const h3 = el("h3", "", title);
    if (powerKey && P(pt, powerKey)) { const b = withInsp(() => toggle(P(pt, powerKey), "", "pw")); h3.append(b); }
    sec.append(h3);
    if (id === "eq" && t >= 0) sec.append(eqEditor(t));
    for (const m of modules) for (const s of mods[m] || []) {
      if (powerKey && `${s.module}.${s.name}` === powerKey) continue;
      sec.append(withInsp(() => paramRow(s, ROWLABEL[`${s.module}.${s.name}`] || (s.module === "eq" ? s.name.replace(/_/g, " ") : undefined))));
    }
    box.append(sec);
  }
}
// Marks the bindings created while building one inspector control, so the next rebuild drops them.
function withInsp(make) {
  const before = new Map(Object.entries(controls).map(([k, a]) => [k, a.length]));
  const e = make();
  for (const [k, a] of Object.entries(controls)) for (let j = before.get(k) || 0; j < a.length; ++j) a[j].insp = true;
  return e;
}
function clipSection(t) {
  const sec = el("div", "sec"); sec.dataset.sec = "clip";
  const clips = E[t] || [];
  const k = sel.clip;
  if (k === null || !clips[k]) {
    sec.append(el("h3", "", "clip"), el("div", "empty", `${clips.length} clip${clips.length === 1 ? "" : "s"}. Click one in the Tracks view to edit it.`));
    return sec;
  }
  sec.append(el("h3", "", `clip ${k + 1} of ${clips.length}`));
  const c = clips[k];
  const field = (label, key, min, max, unit, step) => {
    const spec = { min, max, def: key === "gain" ? 0 : key === "fadeIn" || key === "fadeOut" ? 0 : c[key], unit, step, kind: "float",
      get: () => (E[t][k] || c)[key],
      set: (v, final) => { const before = E[t].map(x => ({ ...x })); E[t][k] = { ...E[t][k], [key]: v }; fixClip(t, k, key); drawLanes(); out.textContent = fmt(spec, E[t][k][key]); if (final) postClips(t, E[t], { before }); } };
    const r = el("div", "prow"); r.append(el("label", "", label));
    const out = el("output", "", fmt(spec, c[key]));
    r.append(slider(spec), out);
    return r;
  };
  const fl = trackLen(c.srcTrack ?? t);
  if ((c.srcTrack ?? t) !== t) sec.append(el("div", "empty", `Audio from ${S.tracks[c.srcTrack].name}`));
  sec.append(field("position", "pos", 0, songLen(), "s", 0.01), field("start in file", "src", 0, fl, "s", 0.01), field("length", "len", 0.05, fl, "s", 0.01),
    field("clip gain", "gain", -24, 12, "dB", 0.1), field("fade in", "fadeIn", 0, Math.min(10, c.len / 2), "s", 0.01), field("fade out", "fadeOut", 0, Math.min(10, c.len / 2), "s", 0.01));
  const acts = el("div", "prow");
  const split = el("button", "", "Split at playhead"); split.onclick = () => splitAtPlayhead();
  const del = el("button", "", "Delete clip"); del.onclick = () => deleteClip();
  acts.append(split, del); sec.append(acts);
  return sec;
}
// Keep a clip consistent after one field changed (inside the file, no overlap, fades fit).
function fixClip(t, k, key) {
  const c = E[t][k], fl = trackLen(c.srcTrack ?? t);
  c.src = clamp(c.src, 0, fl - 0.05);
  c.len = clamp(c.len, 0.05, fl - c.src);
  if (key === "pos") c.pos = Math.max(0, c.pos);
  c.fadeIn = clamp(c.fadeIn, 0, c.len / 2); c.fadeOut = clamp(c.fadeOut, 0, c.len / 2);
}
function eqEditor(t) {
  const c = el("canvas", "eqbig"); c.dataset.curve = t;
  requestAnimationFrame(() => drawCurve(c, t, { big: true }));
  const yToDb = (y, h) => clamp(-(y - h / 2) / (h / 2 - 3) * 18, -18, 18);
  const pick = (x, y) => {
    let best = -1, bd = 14;
    BANDS.forEach((b, k) => {
      if (!pon(t, `eq.${b}_on`)) return;
      const bx = fToX(pv(t, `eq.${b}_freq`), c.clientWidth), by = c.clientHeight / 2 - pv(t, `eq.${b}_gain`) / 18 * (c.clientHeight / 2 - 3);
      const d = Math.hypot(bx - x, by - y); if (d < bd) { bd = d; best = k; }
    });
    return best;
  };
  c.title = "Drag a band: frequency and gain. Wheel: Q. Double-click: gain to 0.";
  c.onpointerdown = ev => {
    const r = c.getBoundingClientRect(), k = pick(ev.clientX - r.left, ev.clientY - r.top);
    if (k < 0) return;
    if (!pon(t, "eq.on")) setParam(P(t, "eq.on").i, 1);
    c.setPointerCapture(ev.pointerId);
    const fs = P(t, `eq.${BANDS[k]}_freq`), gs = P(t, `eq.${BANDS[k]}_gain`), f0 = V[fs.i], g0 = V[gs.i];
    c.onpointermove = m => {
      const x = m.clientX - r.left, y = m.clientY - r.top;
      setParam(fs.i, clamp(xToF(x, r.width), fs.min, fs.max), { record: false });
      setParam(gs.i, Math.round(yToDb(y, r.height) * 10) / 10, { record: false });
      showTip(m, `${BANDS[k].toUpperCase()} ${fmt(fs, V[fs.i])} · ${fmtDb(V[gs.i])} dB`);
    };
    c.onpointerup = () => {
      c.onpointermove = null; tip.hidden = true;
      if (V[fs.i] !== f0 || V[gs.i] !== g0) pushUndo({ kind: "params", list: [{ i: fs.i, from: f0, to: V[fs.i] }, { i: gs.i, from: g0, to: V[gs.i] }], label: `${S.tracks[t].name} EQ ${BANDS[k].toUpperCase()}` });
    };
  };
  c.onwheel = ev => {
    const r = c.getBoundingClientRect(), k = pick(ev.clientX - r.left, ev.clientY - r.top);
    if (k < 0) return;
    ev.preventDefault();
    const qs = P(t, `eq.${BANDS[k]}_q`), from = V[qs.i];
    const v = clamp(from * Math.pow(1.1, ev.deltaY > 0 ? -1 : 1), qs.min, qs.max);
    setParam(qs.i, Math.round(v * 100) / 100, { record: false });
    pushUndo({ kind: "params", list: [{ i: qs.i, from, to: V[qs.i] }], label: "EQ Q", merge: `q${qs.i}` });
    showTip(ev, `Q ${V[qs.i].toFixed(2)}`); clearTimeout(c.tt); c.tt = setTimeout(() => { tip.hidden = true; }, 700);
  };
  c.ondblclick = ev => {
    const r = c.getBoundingClientRect(), k = pick(ev.clientX - r.left, ev.clientY - r.top);
    if (k >= 0) setParam(P(t, `eq.${BANDS[k]}_gain`).i, 0);
  };
  return c;
}

// ---- clips: server sync --------------------------------------------------------------------------------------------
const defaultClips = t => [{ pos: 0, src: 0, len: trackLen(t), gain: 0, fadeIn: 0, fadeOut: 0 }];
let clipsServer = true;
async function loadClips() {
  try { const r = await api("edits"); E = S.tracks.map((_, t) => (r.tracks && Array.isArray(r.tracks[t]) ? r.tracks[t] : defaultClips(t))); }
  catch (_) { clipsServer = false; E = S.tracks.map((_, t) => defaultClips(t)); }
}
async function postClips(t, regions, { record = true, before = null } = {}) {
  const prev = before || E[t].map(x => ({ ...x }));
  E[t] = regions.map(x => ({ ...x }));
  drawLanes();
  if (record) pushUndo({ kind: "clips", track: t, before: prev, after: E[t].map(x => ({ ...x })), label: `${S.tracks[t].name} clip edit` });
  if (!clipsServer) { toast("This mixer build does not support clip edits yet", true); return; }
  try {
    const r = await api("edits", { track: t, regions: E[t] });
    const stored = Array.isArray(r) ? r : r.regions || (r.tracks && r.tracks[t]);
    if (stored) { E[t] = stored; if (sel.track === t && sel.clip !== null && sel.clip >= stored.length) sel.clip = stored.length - 1; drawLanes(); }
  } catch (e) { toast(`Clip edit refused: ${e.message}`, true); }
  if (sel.track === t) refreshClipSection();
}
function refreshClipSection() {
  const old = document.querySelector('#inspector [data-sec="clip"]');
  if (old && sel.track >= 0) old.replaceWith(clipSection(sel.track));
}
function splitAtPlayhead() {
  const t = sel.track;
  if (t === null || t < 0) return toast("Select a track first");
  const x = ST.pos, clips = E[t];
  const k = clips.findIndex(c => x > c.pos + 0.02 && x < c.pos + c.len - 0.02);
  if (k < 0) return toast("No clip under the playhead on this track");
  splitAt(t, k, x);
}
// Clipboard: Cmd+C / Cmd+X copy (cut) the selected clip; Cmd+V pastes it at the playhead (on the grid when Snap is
// on) onto the selected track, any track. The pasted clip replaces what is under it, with short crossfades at both ends.
let CLIPBOARD = null;
const XF = 0.02;
function copyClip(cut) {
  const t = sel.track, k = sel.clip;
  if (t === null || t < 0 || k === null || !E[t][k]) return toast("Select a clip first (click it in the Tracks view)");
  CLIPBOARD = { ...E[t][k], srcTrack: E[t][k].srcTrack ?? t };
  toast(cut ? "Cut" : "Copied: paste with " + MOD + "+V at the playhead");
  if (cut) deleteClip();
}
function pasteClip() {
  if (!CLIPBOARD) return toast("Nothing to paste: copy a clip first");
  const t = sel.track;
  if (t === null || t < 0) return toast("Select the track to paste on");
  let p = Math.max(0, ST.pos || 0);
  if (SNAP.on) p = Math.max(0, quantize(p));
  const nc = { ...CLIPBOARD, pos: p, fadeIn: Math.max(CLIPBOARD.fadeIn || 0, XF), fadeOut: Math.max(CLIPBOARD.fadeOut || 0, XF) };
  if (nc.srcTrack === t) delete nc.srcTrack;
  const a = p, b = p + nc.len, out = [];
  for (const c of E[t]) {
    const ce = c.pos + c.len;
    if (ce <= a || c.pos >= b) { out.push({ ...c }); continue; }
    if (c.pos < a) { const L = Math.min(c.len, a + XF - c.pos); out.push({ ...c, len: L, fadeOut: Math.min(XF, L / 2) }); }
    if (ce > b) { const st = Math.max(c.pos, b - XF); out.push({ ...c, pos: st, src: c.src + (st - c.pos), len: ce - st, fadeIn: Math.min(XF, (ce - st) / 2) }); }
  }
  out.push(nc);
  out.sort((x, y) => x.pos - y.pos);
  sel.clip = out.indexOf(nc);
  postClips(t, out);
  toast(`Pasted at ${fmtPos(p)}`);
}
function deleteClip() {
  const t = sel.track, k = sel.clip;
  if (t === null || t < 0 || k === null || !E[t][k]) return;
  postClips(t, E[t].filter((_, j) => j !== k));
  sel.clip = null;
  refreshClipSection();
}

// ---- tempo and grid ---------------------------------------------------------------------------------------------------
let TEMPO = { bpm: 120, beats_per_bar: 4, offset_s: 0, detected: false };
let SNAP = { on: true, grid: "beat" };
try { Object.assign(SNAP, JSON.parse(localStorage.getItem("mfa.snap") || "{}")); } catch (_) {}
let clockBars = true;
try { clockBars = localStorage.getItem("mfa.clock") !== "time"; } catch (_) {}
const beatSec = () => 60 / (TEMPO.bpm || 120);
const barSec = () => beatSec() * (TEMPO.beats_per_bar || 4);
const gridSec = () => SNAP.grid === "bar" ? barSec() : SNAP.grid === "beat" ? beatSec() : beatSec() * +SNAP.grid;
function toBars(s) {
  const b = (s - TEMPO.offset_s) / beatSec(), bpb = TEMPO.beats_per_bar || 4;
  const bar = Math.floor(b / bpb), beat = b - bar * bpb;
  return { bar: bar + 1, beat: Math.floor(beat) + 1, frac: beat - Math.floor(beat) };
}
const fmtBars = s => { const x = toBars(s); return `${x.bar}.${x.beat}.${String(Math.floor(x.frac * 100)).padStart(2, "0")}`; };
const fmtPos = s => (clockBars ? fmtBars(s) : fmtTime(s));
const quantize = t => TEMPO.offset_s + Math.round((t - TEMPO.offset_s) / gridSec()) * gridSec();
async function loadTempo() {
  try { const r = await api("tempo"); if (r && r.bpm) TEMPO = { ...TEMPO, ...r }; else if (r) TEMPO.state = r.state; } catch (_) {}
  paintTempo(); redrawAll();
  if (TEMPO.state === "detecting") setTimeout(loadTempo, 1000);
}
function setTempo(patch) {
  TEMPO = { ...TEMPO, ...patch, detected: false };
  paintTempo(); redrawAll();
  api("tempo", patch).then(r => { if (r && r.bpm) { TEMPO = { ...TEMPO, ...r }; paintTempo(); redrawAll(); } }).catch(e => toast(`Tempo: ${e.message}`, true));
}
function paintTempo() {
  $("bpm").textContent = TEMPO.state === "detecting" ? "…" : (+TEMPO.bpm).toFixed(1);
  $("sig").textContent = `${TEMPO.beats_per_bar}/4`;
  $("bpm").title = (TEMPO.detected ? `Detected from the drums${TEMPO.confidence != null ? ` (confidence ${Math.round(TEMPO.confidence * 100)}%)` : ""}. ` : "")
    + "Click to type a tempo; press T on the beat to tap it.";
  $("snapBtn").classList.toggle("on", SNAP.on);
  $("gridSel").value = SNAP.grid;
}
function saveSnap() { try { localStorage.setItem("mfa.snap", JSON.stringify(SNAP)); } catch (_) {} paintTempo(); }
let taps = [];
function tapTempo() {
  const now = performance.now();
  taps = taps.filter(x => now - x < 2500); taps.push(now);
  if (taps.length >= 4) { const d = (taps[taps.length - 1] - taps[0]) / (taps.length - 1); setTempo({ bpm: Math.round(600000 / d) / 10 }); toast(`Tempo ${(60000 / d).toFixed(1)} BPM`); }
  else toast(`Tap ${4 - taps.length} more on the beat`);
}
function editBpm() {
  const b = $("bpm"), inp = el("input"); inp.type = "text"; inp.value = (+TEMPO.bpm).toFixed(1);
  b.replaceChildren(inp); inp.focus(); inp.select();
  let done = false;
  const fin = ok => {
    if (done) return; done = true;
    const v = parseFloat(inp.value);
    if (ok && v >= 30 && v <= 300) setTempo({ bpm: v }); else paintTempo();
  };
  inp.onkeydown = e => { e.stopPropagation(); if (e.key === "Enter") fin(true); if (e.key === "Escape") fin(false); };
  inp.onblur = () => fin(true);
}
function initTempo() {
  $("bpm").onclick = editBpm;
  $("sig").onclick = () => { const n = { 2: 3, 3: 4, 4: 5, 5: 6, 6: 7, 7: 2 }[TEMPO.beats_per_bar] || 4; setTempo({ beats_per_bar: n }); };
  $("sig").title = "Beats per bar (click to change)";
  $("snapBtn").onclick = () => { SNAP.on = !SNAP.on; saveSnap(); };
  $("gridSel").onchange = () => { SNAP.grid = $("gridSel").value; SNAP.on = true; saveSnap(); redrawAll(); };
  const bar1 = el("button", "", "1|"); bar1.title = "Put bar 1 at the playhead (for songs with a pickup)";
  bar1.onclick = () => setTempo({ offset_s: Math.round((ST.pos || 0) * 1000) / 1000 });
  const det = el("button", "", "detect"); det.title = "Detect the tempo again from the drums";
  det.onclick = () => api("tempo", { redetect: true }).then(r => { if (r && r.bpm) { TEMPO = { ...TEMPO, ...r }; paintTempo(); redrawAll(); toast(`Tempo ${(+r.bpm).toFixed(1)} BPM`); } }).catch(e => toast(e.message, true));
  for (const b of [bar1, det]) { b.style.padding = "0 5px"; b.style.fontSize = "11px"; }
  $("tempoBox").append(bar1, det);
  $("clock").onclick = () => { clockBars = !clockBars; try { localStorage.setItem("mfa.clock", clockBars ? "bars" : "time"); } catch (_) {} redrawAll(); paintTransport(); };
  paintTempo();
}

// ---- arrangement ---------------------------------------------------------------------------------------------------------
const AR = { pps: 4, x0: 0, laneH: 66, sy: 0, w: 0, h: 0, fitted: false };
const tx = t => (t - AR.x0) * AR.pps;
const xt = x => AR.x0 + x / AR.pps;
function arrSize() {
  const w = $("lanesWrap");
  AR.w = w.clientWidth; AR.h = w.clientHeight;
  for (const c of [$("lanes"), $("overlay")]) { c.style.width = `${AR.w}px`; c.style.height = `${AR.h}px`; }
  $("ruler").style.width = `${AR.w}px`; $("ruler").style.height = "28px";
}
const PAD = 14; // px of timeline before 0:00, so a clip's left edge can be grabbed
function zoomFit() { arrSize(); if (!AR.w) return; AR.pps = (AR.w - PAD) / (songLen() * 1.02); AR.x0 = -PAD / AR.pps; AR.fitted = true; redrawAll(); }
function zoomAt(f, x) {
  const t = xt(x);
  AR.fitted = false;
  AR.pps = clamp(AR.pps * f, AR.w / (songLen() * 1.5), 4000);
  AR.x0 = t - x / AR.pps; clampView(); redrawAll();
}
function clampView() {
  const vis = AR.w / AR.pps;
  AR.x0 = clamp(AR.x0, -PAD / AR.pps, Math.max(-PAD / AR.pps, songLen() * 1.02 - vis));
  AR.sy = clamp(AR.sy, 0, Math.max(0, nTracks() * AR.laneH - AR.h + 20));
}
function ensureTrackVisible(t) {
  const y = t * AR.laneH;
  if (y < AR.sy) AR.sy = y; else if (y + AR.laneH > AR.sy + AR.h) AR.sy = y + AR.laneH - AR.h;
  clampView(); placeHeaders(); drawLanes();
}
function redrawAll() { placeHeaders(); drawRuler(); drawLanes(); drawOverlay(); drawHbar(); }
function tickStep() {
  const minPx = 70;
  for (const s of [0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300]) if (s * AR.pps >= minPx) return s;
  return 600;
}
// Calls fn(x, kind, bar) for the visible bar/beat lines: kind "label" (a numbered bar), "bar", or "beat".
function forGrid(w, fn) {
  const bs = barSec(), be = beatSec(), bpb = TEMPO.beats_per_bar || 4;
  let every = 1;
  while (every * bs * AR.pps < 46 && every < 256) every *= 2;
  const showBeats = be * AR.pps >= 9;
  const first = Math.floor((AR.x0 - TEMPO.offset_s) / bs) - 1;
  for (let k = first; ; ++k) {
    const t = TEMPO.offset_s + k * bs, x = Math.round(tx(t)) + .5;
    if (x > w) break;
    if (x >= -1) fn(x, (k % every + every) % every === 0 ? "label" : "bar", k + 1);
    if (showBeats) for (let j = 1; j < bpb; ++j) { const xb = Math.round(tx(t + j * be)) + .5; if (xb >= 0 && xb <= w) fn(xb, "beat", k + 1); }
  }
}
// What the agent's next turn analyzes: the whole song, the cycle region (>= 2 s), else the excerpt.
function analysisRange() {
  if (ST.analysis_range && ST.analysis_range[1] > ST.analysis_range[0]) return ST.analysis_range;
  const c = ST.loop_region || [0, 0];
  return c[1] - c[0] >= 2 ? c : ST.excerpt || [0, 0];
}
function drawRuler() {
  const c = $("ruler"), [g, w, h] = sizeCanvas(c);
  g.clearRect(0, 0, w, h);
  g.font = "10px -apple-system, sans-serif"; g.textBaseline = "top";
  if (clockBars) {
    forGrid(w, (x, kind, bar) => {
      if (kind === "label") { g.strokeStyle = "#4a515c"; g.beginPath(); g.moveTo(x, 13); g.lineTo(x, h); g.stroke(); g.fillStyle = "#8d95a0"; g.fillText(String(bar), x + 3, 14); }
      else if (kind === "bar") { g.strokeStyle = "#3a414b"; g.beginPath(); g.moveTo(x, 20); g.lineTo(x, h); g.stroke(); }
      else { g.strokeStyle = "#2c323a"; g.beginPath(); g.moveTo(x, 24); g.lineTo(x, h); g.stroke(); }
    });
  } else {
    const st = tickStep(), t0 = Math.floor(AR.x0 / st) * st;
    for (let t = t0; tx(t) < w; t += st) {
      const x = Math.round(tx(t)) + .5;
      g.strokeStyle = "#4a515c"; g.beginPath(); g.moveTo(x, 14); g.lineTo(x, h); g.stroke();
      g.fillStyle = "#8d95a0"; g.fillText(st < 1 ? `${Math.floor(t / 60)}:${(t % 60).toFixed(st < 0.5 ? 2 : 1).padStart(st < 0.5 ? 5 : 4, "0")}` : fmtTime(t).replace(/\.0$/, ""), x + 3, 14);
      for (let k = 1; k < 4; ++k) { const xx = Math.round(tx(t + st * k / 4)) + .5; g.strokeStyle = "#343a43"; g.beginPath(); g.moveTo(xx, 22); g.lineTo(xx, h); g.stroke(); }
    }
  }
  // cycle region
  const [a, b] = AR.dragCycle || ST.loop_region || [0, 0];
  if (b > a) {
    const xa = tx(a), xb = tx(b);
    g.fillStyle = ST.looping || AR.dragCycle ? "#f0a64055" : "#8d95a033";
    g.fillRect(xa, 1, xb - xa, 11);
    g.fillStyle = ST.looping || AR.dragCycle ? "#f0a640" : "#8d95a0";
    g.fillRect(xa, 1, 2, 11); g.fillRect(xb - 2, 1, 2, 11);
  }
  // what the agent analyzes (teal band along the bottom of the ruler)
  const ar = analysisRange();
  if (ar[1] > ar[0]) {
    const xa = tx(ar[0]), xb = tx(ar[1]);
    g.fillStyle = "#4fd1c5"; g.fillRect(xa, h - 4, xb - xa, 4);
  }
  const px = tx(ST.pos || 0);
  g.fillStyle = "#e6e8eb"; g.beginPath(); g.moveTo(px - 5, 0); g.lineTo(px + 5, 0); g.lineTo(px, 7); g.fill();
}
function drawHbar() {
  const bar = $("hbar"), th = bar.firstElementChild, W = bar.clientWidth, L = songLen() * 1.02;
  const vis = AR.w / AR.pps;
  th.style.left = `${clamp(AR.x0 / L, 0, 1) * W}px`; th.style.width = `${clamp(vis / L, 0, 1) * W}px`;
}
function regionAt(t, time) { return (E[t] || []).findIndex(c => time >= c.pos && time < c.pos + c.len); }
function drawLanes() {
  if (!S || view !== "tracks") return;
  const c = $("lanes"), [g, w, h] = sizeCanvas(c);
  g.clearRect(0, 0, w, h);
  const rate = S.rate || 48000, pb = S.peaks_block || 256;
  g.lineWidth = 1;
  if (clockBars) forGrid(w, (x, kind) => { g.strokeStyle = kind === "beat" ? "#191c21" : "#232830"; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke(); });
  else {
    const st = tickStep(), t0 = Math.floor(AR.x0 / st) * st;
    g.strokeStyle = "#1c2026";
    for (let t = t0; tx(t) < w; t += st) { const x = Math.round(tx(t)) + .5; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke(); }
  }
  const [la, lb] = AR.dragCycle || ST.loop_region || [0, 0];
  if (ST.looping && lb > la) { g.fillStyle = "#f0a6400c"; g.fillRect(tx(la), 0, tx(lb) - tx(la), h); }
  const n = nTracks(), H = AR.laneH;
  const anySolo = S.tracks.some((_, t) => pon(t, "solo.on"));
  for (let t = Math.max(0, Math.floor(AR.sy / H)); t < n; ++t) {
    const y = t * H - AR.sy;
    if (y > h) break;
    if (t === sel.track) { g.fillStyle = "#ffffff08"; g.fillRect(0, y, w, H); }
    g.fillStyle = "#23272e"; g.fillRect(0, y + H - 1, w, 1);
    const col = gcol(S.tracks[t].group), silent = pon(t, "mute.on") || (anySolo && !pon(t, "solo.on"));
    (E[t] || []).forEach((cl, k) => {
      const src = cl.srcTrack ?? t, peaks = PK[src];
      const x1 = tx(cl.pos), x2 = tx(cl.pos + cl.len);
      if (x2 < 0 || x1 > w) return;
      const top = y + 2, ch = H - 5, mid = top + ch / 2, selC = t === sel.track && k === sel.clip;
      g.globalAlpha = silent ? 0.35 : 1;
      g.fillStyle = col + "2e"; g.fillRect(x1, top, x2 - x1, ch);
      // waveform
      if (peaks) {
        g.fillStyle = col + "d0";
        const gl = dbToLin(cl.gain || 0), a0 = Math.max(0, Math.floor(x1)), a1 = Math.min(w, Math.ceil(x2));
        for (let x = a0; x < a1; ++x) {
          const ta = xt(x), tb = xt(x + 1);
          const sa = cl.src + (ta - cl.pos), sb = cl.src + (tb - cl.pos);
          let b0 = Math.floor(sa * rate / pb), b1 = Math.max(b0 + 1, Math.ceil(sb * rate / pb));
          b0 = Math.max(0, b0); b1 = Math.min(peaks.length, b1);
          let m = 0;
          for (let b = b0; b < b1; ++b) if (peaks[b] > m) m = peaks[b];
          if (!m) continue;
          let env = 1;
          const into = ta - cl.pos, left = cl.pos + cl.len - ta;
          if (cl.fadeIn > 0 && into < cl.fadeIn) env = Math.sin(Math.PI / 2 * clamp(into / cl.fadeIn, 0, 1));
          if (cl.fadeOut > 0 && left < cl.fadeOut) env *= Math.sin(Math.PI / 2 * clamp(left / cl.fadeOut, 0, 1));
          const amp = Math.min(1, (m / 255) * Math.sqrt(gl * env)) * (ch / 2 - 2);
          g.fillRect(x, mid - amp, 1, Math.max(1, amp * 2));
        }
      }
      // fades
      g.strokeStyle = "#ffffffa0"; g.lineWidth = 1;
      if (cl.fadeIn > 0) { const xf = tx(cl.pos + cl.fadeIn); g.beginPath(); for (let k2 = 0; k2 <= 16; ++k2) { const u = k2 / 16; const xx = x1 + (xf - x1) * u, yy = top + ch - ch * Math.sin(Math.PI / 2 * u); k2 ? g.lineTo(xx, yy) : g.moveTo(xx, yy); } g.stroke(); }
      if (cl.fadeOut > 0) { const xf = tx(cl.pos + cl.len - cl.fadeOut); g.beginPath(); for (let k2 = 0; k2 <= 16; ++k2) { const u = k2 / 16; const xx = xf + (x2 - xf) * u, yy = top + ch - ch * Math.cos(Math.PI / 2 * u); k2 ? g.lineTo(xx, yy) : g.moveTo(xx, yy); } g.stroke(); }
      // frame, handles, labels
      g.strokeStyle = selC ? "#ffffff" : col; g.lineWidth = selC ? 1.5 : 1;
      g.strokeRect(x1 + .5, top + .5, x2 - x1 - 1, ch - 1);
      if (selC || H >= 50) {
        g.fillStyle = "#ffffffd0";
        for (const hx of [tx(cl.pos + cl.fadeIn), tx(cl.pos + cl.len - cl.fadeOut)]) if (hx >= x1 - 1 && hx <= x2 + 1) g.fillRect(hx - 3, top + 1, 6, 5);
      }
      g.save(); g.beginPath(); g.rect(x1, top, x2 - x1, ch); g.clip();
      g.font = "10px -apple-system, sans-serif"; g.textBaseline = "top"; g.fillStyle = "#e6e8ebcc";
      const label = src !== t ? `${S.tracks[src].name} (pasted)` : k ? `${S.tracks[t].name} (${k + 1})` : S.tracks[t].name;
      if (x2 - x1 > 50) g.fillText(label, Math.max(x1, 0) + 4, top + 7);
      if (Math.abs(cl.gain || 0) >= 0.05 && x2 - x1 > 80) { const s2 = `${fmtDb(cl.gain)} dB`; g.fillText(s2, Math.min(x2, w) - g.measureText(s2).width - 5, top + 7); }
      g.restore();
      if (H >= 40 && x2 - x1 > 24) { // clip gain: line + knob, dragged up and down
        const kn = gainKnob(cl, y, x1, x2);
        g.strokeStyle = Math.abs(cl.gain || 0) >= 0.05 ? "#ffd27a" : "#ffffff40"; g.lineWidth = 1;
        g.beginPath(); g.moveTo(Math.max(x1, 0) + 2, kn.y + .5); g.lineTo(Math.min(x2, w) - 2, kn.y + .5); g.stroke();
        g.beginPath(); g.arc(kn.x, kn.y, 4, 0, 7); g.fillStyle = Math.abs(cl.gain || 0) >= 0.05 ? "#ffd27a" : "#cfd3d9"; g.fill();
      }
      g.globalAlpha = 1;
    });
  }
}
function drawOverlay() {
  if (view !== "tracks") return;
  const c = $("overlay"), [g, w, h] = sizeCanvas(c);
  g.clearRect(0, 0, w, h);
  const x = Math.round(tx(ST.pos || 0)) + .5;
  if (x >= 0 && x <= w) { g.strokeStyle = "#e6e8eb"; g.lineWidth = 1; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke(); }
  const tu = ST.turn;
  if (tu && tu.stage === "listening") { // the agent's background analysis sweeping its range
    const r = tu.range || analysisRange(), f = clamp((tu.listen_s || 0) / Math.max(0.1, tu.listen_need_s || r[1] - r[0]), 0, 1);
    const xa = tx(r[0]), xb = tx(r[1]), xs = xa + (xb - xa) * f;
    g.fillStyle = "#4fd1c51c"; g.fillRect(xa, 0, xs - xa, h);
    g.strokeStyle = "#4fd1c5"; g.beginPath(); g.moveTo(xs + .5, 0); g.lineTo(xs + .5, h); g.stroke();
  }
  if (AR.snapX !== undefined) { g.strokeStyle = "#4fd1c5"; g.setLineDash([3, 3]); g.beginPath(); g.moveTo(AR.snapX + .5, 0); g.lineTo(AR.snapX + .5, h); g.stroke(); g.setLineDash([]); }
}
// Track headers: DOM rows that scroll with the lanes.
function buildHeaders() {
  const box = $("headersIn");
  for (const k of Object.keys(controls)) controls[k] = controls[k].filter(f => !f.hdr);
  box.replaceChildren();
  S.tracks.forEach((tr, t) => {
    const h = el("div", "th"); h.dataset.t = t;
    const gc = el("i", "gc"); gc.style.background = gcol(tr.group);
    const nm = el("span", "nm", tr.name); nm.title = `${tr.name} (double-click to rename)`;
    nm.ondblclick = ev => { ev.stopPropagation(); rename(t, nm); };
    const ms = el("span", "ms");
    const mk = () => {
      ms.append(toggle(P(t, "mute.on"), "M", "m"), toggle(P(t, "solo.on"), "S", "s"));
      const r2 = el("div", "row2");
      const fs = P(t, "fader.gain"), val = el("span", "val");
      r2.append(slider(fs), slider(P(t, "balance.value")), val);
      bind(fs.i, v => { val.textContent = `${fmtDb(v, true)} · ${panTxt(pv(t, "balance.value"))}`; });
      bind(P(t, "balance.value").i, () => { val.textContent = `${fmtDb(pv(t, "fader.gain"), true)} · ${panTxt(pv(t, "balance.value"))}`; });
      return r2;
    };
    const before = new Map(Object.entries(controls).map(([k, a]) => [k, a.length]));
    const r2 = mk();
    for (const [k, a] of Object.entries(controls)) for (let j = before.get(k) || 0; j < a.length; ++j) a[j].hdr = true;
    const mt = el("div", "mt"); const mi = el("i"); mt.append(mi);
    const dots = el("div", "dots"); dots.dataset.dots = t;
    h.append(gc, el("span", "num", t + 1), nm, ms, mt, r2, dots);
    h.meter = mi;
    h.onpointerdown = () => selectTrack(t);
    box.append(h);
  });
  placeHeaders();
  S.tracks.forEach((_, t) => updateDots(t));
}
const DOT = { filters: "FLT", gate: "GATE", eq: "EQ", comp: "COMP", deesser: "DS", drive: "DRV", doubler: "DBL", sends: "FX" };
function updateDots(t) {
  const d = document.querySelector(`[data-dots="${t}"]`);
  if (!d || t < 0) return;
  d.replaceChildren();
  for (const b of trackBlocks(t)) {
    if (["pan", "bus", "input"].includes(b.id)) continue;
    const x = el("span", "dot" + (b.on ? " on" : ""), DOT[b.id] || b.label.toUpperCase());
    if (b.on) x.style.background = b.color;
    x.title = `${b.label}: ${b.text}`;
    d.append(x);
  }
}
function placeHeaders() {
  const box = $("headersIn");
  box.style.transform = `translateY(${-AR.sy}px)`;
  box.style.height = `${nTracks() * AR.laneH}px`;
  [...box.children].forEach((h, t) => { h.style.top = `${t * AR.laneH}px`; h.style.height = `${AR.laneH}px`; h.querySelector(".dots").style.display = AR.laneH >= 58 ? "" : "none"; h.querySelector(".row2").style.display = AR.laneH >= 40 ? "" : "none"; });
}
function rename(t, elx) {
  const inp = el("input"); inp.type = "text"; inp.value = S.tracks[t].name; inp.style.width = "100%";
  elx.textContent = ""; elx.append(inp); inp.focus(); inp.select();
  let finished = false;
  const done = async ok => {
    if (finished) return;
    finished = true;
    const v = inp.value.trim();
    elx.textContent = S.tracks[t].name;
    if (!ok || !v || v === S.tracks[t].name) return;
    try { await api("track", { index: t, name: v }); setTimeout(reloadSession, 150); } catch (e) { toast(e.message, true); }
  };
  inp.onkeydown = e => { e.stopPropagation(); if (e.key === "Enter") done(true); if (e.key === "Escape") done(false); };
  inp.onblur = () => done(true);
  inp.onpointerdown = e => e.stopPropagation();
}
// Tools: the pointer (move, trim, fades, gain) and the scissors (click a clip to split it there). B toggles.
let tool = "pointer";
function setTool(x) {
  tool = x;
  const b = $("scissorsBtn"); if (b) b.classList.toggle("on", tool === "scissors");
  $("overlay").style.cursor = tool === "scissors" ? "crosshair" : "";
  toast(tool === "scissors" ? "Scissors: click a clip to split it (B or Esc for the pointer)" : "Pointer");
}
function splitAt(t, k, time) {
  const clips = E[t], c = clips[k];
  if (!c || time <= c.pos + 0.02 || time >= c.pos + c.len - 0.02) return false;
  const d = time - c.pos;
  const a = { ...c, len: d, fadeIn: Math.min(c.fadeIn, d / 2), fadeOut: 0 };
  const b = { ...c, pos: time, src: c.src + d, len: c.len - d, fadeIn: 0, fadeOut: Math.min(c.fadeOut, (c.len - d) / 2) };
  sel.clip = k + 1;
  postClips(t, [...clips.slice(0, k), a, b, ...clips.slice(k + 1)]);
  return true;
}
// Lane interaction: move, trim, fades, clip gain; click to place the playhead.
const EDGE = 6;
function hitClip(x, y) {
  const t = Math.floor((y + AR.sy) / AR.laneH);
  if (t < 0 || t >= nTracks()) return { t: -1 };
  const yIn = y + AR.sy - t * AR.laneH;
  const clips = E[t] || [];
  for (let k = clips.length - 1; k >= 0; --k) { // later clips are drawn on top
    const c = clips[k], x1 = tx(c.pos), x2 = tx(c.pos + c.len);
    if (x < x1 - EDGE || x > x2 + EDGE) continue;
    if (AR.laneH >= 40) {
      const kn = gainKnob(c, 0, x1, x2);
      if (Math.abs(x - kn.x) <= 9 && Math.abs(yIn - kn.y) <= 7) return { t, k, zone: "gain" };
    }
    if (yIn <= 10) {
      const fi = tx(c.pos + c.fadeIn), fo = tx(c.pos + c.len - c.fadeOut);
      if (Math.abs(x - fi) <= 7) return { t, k, zone: "fadeIn" };
      if (Math.abs(x - fo) <= 7) return { t, k, zone: "fadeOut" };
    }
    if (Math.abs(x - x1) <= EDGE) return { t, k, zone: "trimL" };
    if (Math.abs(x - x2) <= EDGE) return { t, k, zone: "trimR" };
    if (x >= x1 && x <= x2) return { t, k, zone: "move" };
  }
  return { t, k: -1 };
}
// The clip gain handle: a line across the clip at its gain (0 dB in the middle), with a knob in the visible middle.
function gainKnob(c, laneTop, x1, x2) {
  const ch = AR.laneH - 5, mid = laneTop + 2 + ch / 2;
  return { x: (Math.max(x1, 0) + Math.min(x2, AR.w)) / 2, y: mid - clamp(c.gain || 0, -24, 12) / 24 * (ch / 2 - 4) };
}
// Overlapping clips on one track become crossfades: each overlap is faded out on the earlier clip and in on the later.
function crossfade(list) {
  list.sort((a, b) => a.pos - b.pos);
  for (let i = 0; i + 1 < list.length; ++i) {
    const a = list[i], b = list[i + 1], o = a.pos + a.len - b.pos;
    if (o <= 0.001 || b.pos + b.len <= a.pos + a.len) continue;
    a.fadeOut = Math.min(o, a.len / 2); b.fadeIn = Math.min(o, b.len / 2);
  }
  return list;
}
function snapTime(time, t, k, fine) {
  if (fine) return { v: Math.round(time * 1000) / 1000 };
  if (SNAP.on) return { v: Math.max(0, quantize(time)), grid: true };
  const cands = [ST.pos || 0, 0, ...(ST.loop_region || [])];
  E.forEach((cl, tt) => cl.forEach((c, kk) => { if (tt === t && kk === k) return; cands.push(c.pos, c.pos + c.len); }));
  let best = null, bd = 8 / AR.pps;
  for (const c of cands) if (Math.abs(c - time) < bd) { bd = Math.abs(c - time); best = c; }
  if (best !== null) return { v: best, snapped: true };
  return { v: Math.round(time * 100) / 100 };
}
function initLanes() {
  const c = $("overlay");
  const cursorFor = z => ({ fadeIn: "nwse-resize", fadeOut: "nesw-resize", trimL: "ew-resize", trimR: "ew-resize", move: "grab", gain: "ns-resize" })[z] || "text";
  c.onpointermove = ev => {
    if (c.dragging) return;
    if (tool === "scissors") { c.style.cursor = "crosshair"; return; }
    const r = c.getBoundingClientRect(); const h = hitClip(ev.clientX - r.left, ev.clientY - r.top);
    c.style.cursor = ev.altKey && h.zone === "move" ? "ns-resize" : cursorFor(h.zone);
  };
  c.onpointerdown = ev => {
    if (ev.button !== 0) return;
    const r = c.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
    const h = hitClip(x, y);
    if (h.t < 0) return;
    if (h.k < 0) { sel.clip = null; selectTrack(h.t, { keepClip: false }); seek(xt(x)); return; }
    if (tool === "scissors") {
      let at = xt(x);
      if (SNAP.on && !ev.shiftKey) at = quantize(at);
      selectTrack(h.t, { clip: h.k });
      if (!splitAt(h.t, h.k, at)) toast("Too close to the clip's edge to split");
      return;
    }
    selectTrack(h.t, { clip: h.k });
    const t = h.t, k = h.k, before = E[t].map(x2 => ({ ...x2 })), c0 = { ...E[t][k] };
    const zone = ev.altKey && h.zone === "move" ? "gain" : h.zone;
    const fl = trackLen(c0.srcTrack ?? t);
    c.setPointerCapture(ev.pointerId); c.dragging = true;
    let moved = false;
    c.onpointermove = m => {
      const dx = m.clientX - ev.clientX, dy = m.clientY - ev.clientY;
      if (!moved && Math.abs(dx) < 3 && Math.abs(dy) < 3) return;
      moved = true;
      const fine = m.shiftKey, dt = dx / AR.pps * (zone === "gain" ? 0 : 1);
      const cl = { ...c0 };
      AR.snapX = undefined;
      if (zone === "move") {
        let p = c0.pos + dt;
        const s1 = snapTime(p, t, k, fine), s2 = snapTime(p + c0.len, t, k, fine);
        if (s1.snapped) { p = s1.v; AR.snapX = tx(p); } else if (s2.snapped) { p = s2.v - c0.len; AR.snapX = tx(s2.v); } else p = s1.v;
        cl.pos = Math.max(0, p);
        showTip(m, fmtPos(cl.pos));
      } else if (zone === "trimL") {
        const s1 = snapTime(c0.pos + dt, t, k, fine); if (s1.snapped) AR.snapX = tx(s1.v);
        let d = s1.v - c0.pos;
        d = clamp(d, Math.max(-c0.src, -c0.pos), c0.len - 0.05);
        cl.pos = c0.pos + d; cl.src = c0.src + d; cl.len = c0.len - d;
        showTip(m, `start ${fmtPos(cl.pos)} · ${cl.len.toFixed(2)} s`);
      } else if (zone === "trimR") {
        const s1 = snapTime(c0.pos + c0.len + dt, t, k, fine); if (s1.snapped) AR.snapX = tx(s1.v);
        cl.len = clamp(s1.v - c0.pos, 0.05, fl - c0.src);
        showTip(m, `end ${fmtPos(cl.pos + cl.len)} · ${cl.len.toFixed(2)} s`);
      } else if (zone === "fadeIn") {
        cl.fadeIn = clamp(c0.fadeIn + dt, 0, cl.len / 2); showTip(m, `fade in ${cl.fadeIn.toFixed(2)} s`);
      } else if (zone === "fadeOut") {
        cl.fadeOut = clamp(c0.fadeOut - dt, 0, cl.len / 2); showTip(m, `fade out ${cl.fadeOut.toFixed(2)} s`);
      } else if (zone === "gain") {
        cl.gain = clamp(Math.round(((c0.gain || 0) - dy * (fine ? 0.05 : 0.2)) * 10) / 10, -24, 12); showTip(m, `clip gain ${fmtDb(cl.gain)} dB`);
      }
      cl.fadeIn = Math.min(cl.fadeIn, cl.len / 2); cl.fadeOut = Math.min(cl.fadeOut, cl.len / 2);
      E[t][k] = cl;
      drawLanes(); drawOverlay();
    };
    c.onpointerup = () => {
      c.onpointermove = null; c.dragging = false; tip.hidden = true; AR.snapX = undefined; drawOverlay();
      c.onpointermove = initLanes.hover;
      if (moved) {
        const mine = E[t][k];
        crossfade(E[t]);
        sel.clip = E[t].indexOf(mine);
        postClips(t, E[t], { before });
      } else refreshClipSection();
    };
  };
  initLanes.hover = c.onpointermove;
  c.ondblclick = ev => { const r = c.getBoundingClientRect(); const h = hitClip(ev.clientX - r.left, ev.clientY - r.top); if (h.k >= 0) selectTrack(h.t, { clip: h.k, section: "clip" }); };
  const wheel = ev => {
    ev.preventDefault();
    const r = $("lanesWrap").getBoundingClientRect();
    if (ev.ctrlKey || ev.metaKey) { zoomAt(Math.exp(-ev.deltaY * 0.01), ev.clientX - r.left); return; }
    // Over the clips a plain wheel moves along the song (a mouse has only the vertical wheel); Shift+wheel moves
    // up and down the tracks, as does the wheel over the track names.
    let dx, dy;
    if (ev.shiftKey) { dx = 0; dy = ev.deltaY || ev.deltaX; }
    else { dx = ev.deltaX + ev.deltaY; dy = 0; }
    if (dx) AR.fitted = false;
    AR.x0 += dx / AR.pps; AR.sy += dy;
    clampView(); redrawAll();
  };
  $("lanesWrap").addEventListener("wheel", wheel, { passive: false });
  $("headers").addEventListener("wheel", ev => { ev.preventDefault(); AR.sy += ev.deltaY; clampView(); redrawAll(); }, { passive: false });
  $("rulerWrap").addEventListener("wheel", wheel, { passive: false });
  $("mixer").addEventListener("wheel", ev => {
    if (ev.defaultPrevented || ev.ctrlKey || ev.metaKey) return; // a fader or knob took the wheel
    ev.preventDefault();
    $("mixer").scrollLeft += ev.shiftKey ? ev.deltaY : ev.deltaX + ev.deltaY;
  }, { passive: false });
  // ruler: click seeks, drag sets the cycle
  const ru = $("rulerWrap");
  ru.onpointerdown = ev => {
    const r = ru.getBoundingClientRect(), t0 = xt(ev.clientX - r.left);
    ru.setPointerCapture(ev.pointerId);
    let moved = false;
    ru.onpointermove = m => {
      const t1 = xt(m.clientX - r.left);
      if (Math.abs(m.clientX - ev.clientX) > 3) moved = true;
      if (moved) { AR.dragCycle = [Math.max(0, Math.min(t0, t1)), Math.min(songLen(), Math.max(t0, t1))]; drawRuler(); drawLanes(); }
    };
    ru.onpointerup = async () => {
      ru.onpointermove = null;
      const d = AR.dragCycle; AR.dragCycle = null;
      if (moved && d && d[1] - d[0] >= 1) await api("transport", { region: d, loop: true, seek: d[0] }).catch(showErr);
      else seek(t0);
      drawRuler(); drawLanes();
    };
  };
  ru.ondblclick = () => cycleToExcerpt();
  // scrollbar
  const hb = $("hbar"), th = hb.firstElementChild;
  th.onpointerdown = ev => {
    th.setPointerCapture(ev.pointerId);
    const x00 = AR.x0, W = hb.clientWidth, L = songLen() * 1.02;
    th.onpointermove = m => { AR.x0 = x00 + (m.clientX - ev.clientX) / W * L; clampView(); redrawAll(); };
    th.onpointerup = () => { th.onpointermove = null; };
  };
  $("hPlus").onclick = () => setLaneH(AR.laneH + 14);
  $("hMinus").onclick = () => setLaneH(AR.laneH - 14);
  new ResizeObserver(() => { const wasFit = AR.fitted; arrSize(); if (wasFit) zoomFit(); else { clampView(); redrawAll(); } }).observe($("lanesWrap"));
}
function setLaneH(h) { AR.laneH = clamp(h, 30, 160); clampView(); redrawAll(); }
const seek = t => api("transport", { seek: clamp(t, 0, songLen()) }).catch(showErr);
const cycleToExcerpt = () => ST.excerpt && api("transport", { region: ST.excerpt, loop: true, seek: ST.excerpt[0] }).catch(showErr);

// ---- mixer view ------------------------------------------------------------------------------------------------------
function buildMixer() {
  const box = $("mixer");
  for (const k of Object.keys(controls)) controls[k] = controls[k].filter(f => !f.mix);
  box.replaceChildren();
  const before = new Map(Object.entries(controls).map(([k, a]) => [k, a.length]));
  S.tracks.forEach((_, t) => box.append(strip(t)));
  // After the tracks: the group buses and effect returns (they scroll with the tracks), then the master, pinned right.
  const sep = el("div", "sepl", "buses · returns"); sep.id = "busStart"; box.append(sep);
  BUSKEYS.forEach((_, g) => box.append(busStrip(g)));
  const pin = el("div", "masterpin"); pin.append(strip(-1)); box.append(pin);
  for (const [k, a] of Object.entries(controls)) for (let j = before.get(k) || 0; j < a.length; ++j) a[j].mix = true;
}
function strip(t) {
  const master = t < 0, e = el("div", "strip" + (master ? " master" : "")); e.dataset.t = t;
  const top = el("div", "top");
  const chip = el("span", "chip", master ? "M" : t + 1); chip.style.background = master ? "#e6e8eb" : gcol(S.tracks[t].group);
  const nm = el("span", "nm", master ? "MASTER" : S.tracks[t].name); nm.title = master ? "Master" : `${S.tracks[t].name} (double-click to rename)`;
  if (!master) nm.ondblclick = ev => { ev.stopPropagation(); rename(t, nm); };
  top.append(chip, nm); e.append(top);
  if (!master) {
    const cv = el("canvas", "curve"); cv.dataset.curve = t; cv.title = "Filter and EQ response. Click to edit.";
    cv.onclick = ev => { ev.stopPropagation(); selectTrack(t, { section: "eq" }); };
    requestAnimationFrame(() => drawCurve(cv, t));
    e.append(cv);
  }
  e.append(blockRows(t));
  if (master) e.append(returnsBox());
  if (!master) {
    const pr = el("div", "panrow"); const pl = el("span", "", "C");
    pr.append(slider(P(t, "balance.value")), pl);
    bind(P(t, "balance.value").i, v => { pl.textContent = panTxt(v); });
    e.append(pr);
  }
  const fz = el("div", "fz");
  const fs = P(t, "fader.gain");
  fz.append(slider(fs, { vertical: true }));
  const mk = cls => { const m = el("div", "mt" + (cls ? " " + cls : "")); const p = el("i"), r = el("i", "r"); m.append(r, p); return [m, p, r]; };
  e.meters = [];
  if (master) { const a = mk(), b = mk(); fz.append(a[0], b[0]); e.meters = [a, b]; }
  else { const a = mk(), b = mk(), gr = mk("gr"); fz.append(a[0], b[0], gr[0]); e.meters = [a, b, gr]; a[0].title = "input peak"; b[0].title = "output peak / RMS"; gr[0].title = "gain reduction (compressor or gate)"; }
  e.append(fz);
  const lb = el("div", "lbls"); lb.innerHTML = master ? "<span>L</span><span>R</span>" : "<span>fdr</span><span>in</span><span>out</span><span>gr</span>";
  e.append(lb);
  const db = el("div", "db"); bind(fs.i, v => { db.textContent = `${fmtDb(v, true)} dB`; }); e.append(db);
  if (master) { const lu = el("div", "db"); lu.dataset.lufs = "1"; e.append(lu); }
  if (!master) { const ms = el("div", "ms"); ms.append(toggle(P(t, "mute.on"), "M", "m"), toggle(P(t, "solo.on"), "S", "s")); e.append(ms); }
  e.onpointerdown = () => selectTrack(t);
  return e;
}
const BUSKEYS = ["vocals", "drums", "bass", "guitars", "keys"];
// Selection ids for things that are not tracks: -1 master, -2..-6 the group buses, -7 reverb, -8 echo.
const BUS_ID = g => -2 - g, REV_ID = -7, ECHO_ID = -8;
const mkMeter = cls => { const m = el("div", "mt" + (cls ? " " + cls : "")); const p = el("i"), r = el("i", "r"); m.append(r, p); return [m, p, r]; };
// A group bus: the tracks routed to it, its compressor and peak clipper, its level and gain reduction.
function busStrip(g) {
  const key = BUSKEYS[g], e = el("div", "strip bus"); e.dataset.t = BUS_ID(g);
  const top = el("div", "top"), chip = el("span", "chip", GSHORT[g]); chip.style.background = GCOL[g];
  top.append(chip, el("span", "nm", `${GROUPS[g]} bus`)); e.append(top);
  const members = S.tracks.map((tr, t) => tr.group === g ? `${t + 1} ${tr.name}` : null).filter(Boolean);
  const mem = el("div", "members", members.length ? members.join("\n") : "no tracks"); mem.style.whiteSpace = "pre"; mem.title = members.join("\n");
  e.append(mem);
  const on = P(-1, `bus_${key}.on`);
  const row = (label, k) => { const r = el("div", "blk"); const i = el("i", "k"); i.style.background = "var(--comp)"; const v = el("span", "v"); r.append(i, el("span", "l", label), v); bind(P(-1, k).i, x => { v.textContent = fmt(P(-1, k), x); r.classList.toggle("off", !(V[on.i] > 0.5)); }); bind(on.i, () => r.classList.toggle("off", !(V[on.i] > 0.5))); return r; };
  const t1 = toggle(on, "", ""); t1.style.width = "100%";
  bind(on.i, v => { t1.textContent = v > 0.5 ? "compressor on" : "compressor off"; });
  e.append(t1, row("thr", `bus_${key}.threshold`), row("ratio", `bus_${key}.ratio`), row("make", `bus_${key}.makeup`), row("clip", `bus_${key}.clip`));
  const fz = el("div", "fz"), lv = mkMeter(), gr = mkMeter("gr");
  lv[0].title = "bus level (peak / RMS)"; gr[0].title = "bus compressor gain reduction";
  fz.append(lv[0], gr[0]); e.append(fz);
  const lb = el("div", "lbls"); lb.innerHTML = "<span>level</span><span>gr</span>"; e.append(lb);
  const db = el("div", "db"); e.append(db);
  e.paint = () => {
    const b0 = (ST.buses || [])[g] || { peak: 0, rms: 0, gr: 0 };
    const sm = e.sm || (e.sm = { peak: 0, rms: 0, gr: 0 }); // per-block values: decay like a meter
    for (const k of ["peak", "rms", "gr"]) sm[k] = Math.max(b0[k] || 0, sm[k] * 0.86);
    const b = sm;
    lv[1].style.height = `${meterH(b.peak)}%`; lv[2].style.height = `${meterH(b.rms)}%`; lv[1].classList.toggle("hot", b.peak >= 0.99);
    gr[1].style.height = `${clamp((b.gr || 0) / 12 * 100, 0, 100)}%`;
    db.textContent = b.gr > 0.05 ? `GR ${b.gr.toFixed(1)} dB` : "";
  };
  e.onpointerdown = () => selectTrack(BUS_ID(g));
  return e;
}
// The effect returns feed the master, so they sit in its strip: return level, a meter, click for their settings.
function returnsBox() {
  const box = el("div", "returns");
  box.append(el("div", "rh", "returns"));
  for (const kind of ["reverb", "echo"]) {
    const r = el("div", "ret"), rs = P(-1, `${kind}.return`);
    const nm = el("span", "rn", kind === "reverb" ? "REV" : "ECHO");
    nm.title = `${kind === "reverb" ? "Reverb" : "Echo"} return: click for its settings`;
    nm.onclick = ev => { ev.stopPropagation(); selectTrack(kind === "reverb" ? REV_ID : ECHO_ID); };
    const val = el("span", "rv"); bind(rs.i, v => { val.textContent = fmtDb(v, true); });
    const mt = el("div", "hm"), bar = el("i"); mt.append(bar); mt.dataset.ret = kind;
    const sends = S.tracks.filter((_, t) => pv(t, kind === "reverb" ? "send.gain" : "echo_send.gain") > -89.9).length;
    r.title = `${sends} track${sends === 1 ? "" : "s"} send to the ${kind}`;
    r.append(nm, slider(rs), val, mt);
    r.onpointerdown = ev => ev.stopPropagation(); // using a return does not select the master
    box.append(r);
  }
  return box;
}
// (unused) An effect return as its own strip.
function fxStrip(kind) {
  const e = el("div", "strip fx"); e.dataset.t = kind === "reverb" ? REV_ID : ECHO_ID;
  const top = el("div", "top"), chip = el("span", "chip", kind === "reverb" ? "REV" : "ECHO"); chip.style.background = "#4fd1c5";
  top.append(chip, el("span", "nm", kind === "reverb" ? "Reverb" : "Echo")); e.append(top);
  const info = el("div", "members");
  const upd = () => { info.textContent = kind === "reverb" ? `${pv(-1, "reverb.decay").toFixed(1)} s decay\nsize ${pv(-1, "reverb.size").toFixed(2)}\npredelay ${pv(-1, "reverb.predelay").toFixed(0)} ms` : `${pv(-1, "echo.time_l").toFixed(0)} / ${pv(-1, "echo.time_r").toFixed(0)} ms\nfeedback ${pv(-1, "echo.feedback").toFixed(2)}`; };
  info.style.whiteSpace = "pre";
  for (const k of kind === "reverb" ? ["reverb.decay", "reverb.size", "reverb.predelay"] : ["echo.time_l", "echo.time_r", "echo.feedback"]) bind(P(-1, k).i, upd);
  const sends = S.tracks.filter((_, t) => pv(t, kind === "reverb" ? "send.gain" : "echo_send.gain") > -89.9).length;
  e.append(info, el("div", "members", `${sends} track${sends === 1 ? "" : "s"} send here`));
  const fz = el("div", "fz"), rs = P(-1, `${kind}.return`), lv = mkMeter();
  fz.append(slider(rs, { vertical: true }), lv[0]); e.append(fz);
  const db = el("div", "db"); bind(rs.i, v => { db.textContent = `return ${fmtDb(v, true)}`; }); e.append(db);
  e.paint = () => { const p = e.pk = Math.max(((ST.fx || {})[kind] || {}).peak || 0, (e.pk || 0) * 0.86); lv[1].style.height = `${meterH(p)}%`; lv[2].style.height = "0"; };
  e.onpointerdown = () => selectTrack(kind === "reverb" ? REV_ID : ECHO_ID);
  return e;
}
const peakDb = x => 20 * Math.log10(Math.max(x, 1e-6));
const meterH = x => clamp((peakDb(x) + 60) / 60 * 100, 0, 100);
function paintMeters() {
  const m = ST.meters || [];
  if (view === "tracks") {
    const hs = $("headersIn").children;
    for (let t = 0; t < hs.length; ++t) { const x = m[t] ? m[t].out : (ST.tracks || [])[t] || 0; if (hs[t].meter) { hs[t].meter.style.height = `${meterH(x)}%`; hs[t].meter.classList.toggle("hot", x >= 0.99); } }
  } else {
    const strips = $("mixer").querySelectorAll(".strip");
    for (let t = 0; t < strips.length; ++t) {
      const s = strips[t], ms = s.meters || [];
      if (s.paint) { s.paint(); continue; }
      if (+s.dataset.t < 0) {
        const mm = ST.master || [0, 0];
        s.querySelectorAll("[data-ret]").forEach(m => {
          const k = m.dataset.ret, p = m.pk = Math.max(((ST.fx || {})[k] || {}).peak || 0, (m.pk || 0) * 0.86);
          m.firstChild.style.width = `${meterH(p)}%`;
        });
        const lu = s.querySelector("[data-lufs]");
        if (lu) lu.textContent = ST.master_lufs_m != null && ST.master_lufs_m > -70 ? `${ST.master_lufs_m.toFixed(1)} LUFS-M` : "";
        ms.forEach((q, k) => { q[1].style.height = `${meterH(mm[k])}%`; q[1].classList.toggle("hot", mm[k] >= 0.99); q[2].style.height = "0"; });
        continue;
      }
      const d = m[t] || { in: 0, out: (ST.tracks || [])[t] || 0, out_rms: 0, gr: 0 };
      if (ms[0]) { ms[0][1].style.height = `${meterH(d.in)}%`; ms[0][2].style.height = "0"; }
      if (ms[1]) { ms[1][1].style.height = `${meterH(d.out)}%`; ms[1][2].style.height = `${meterH(d.out_rms)}%`; ms[1][1].classList.toggle("hot", d.out >= 0.99); }
      if (ms[2]) { ms[2][1].style.height = `${clamp((d.gr || 0) / 24 * 100, 0, 100)}%`; }
    }
  }
}

// ---- assistant ------------------------------------------------------------------------------------------------------
let chatVersion = -1, chat = [];
const STAGES = ["listening", "waiting", "reading", "applying"];
function paramLabel(id) {
  const m = /^t(\d+)\.(.*)$/.exec(id || "");
  const rest = (m ? m[2] : (id || "").replace(/^master\./, "master · ")).replace(/[._]/g, " ");
  return m && S && S.tracks[+m[1] - 1] ? `${S.tracks[+m[1] - 1].name} · ${rest}` : rest;
}
async function pollChat(version) {
  if (version === chatVersion) return;
  const c = await api("chat");
  chatVersion = c.version; chat = c.entries;
  if (cmp && !chat.some(e => e.role === "agent" && e.tick === cmp.tick && !e.undone)) endCompare(false); // the compared turn was undone
  renderChat();
}
function renderChat() {
  const log = $("log");
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  const notes = {};
  for (const e of chat) if (e.role === "note") (notes[e.tick] ||= []).push(e.text);
  const frag = document.createDocumentFragment();
  const live = ST.turn && ST.turn.stage && ST.turn.stage !== "done" ? ST.turn : null;
  for (const e of chat) {
    if (e.role === "note") continue;
    if (e.role === "user") { const m = el("div", "msg user"); m.append(el("span", "who", "you"), el("span", "", e.text)); frag.append(m); continue; }
    if (e.role === "system") {
      if (/^Waiting for your coding agent/.test(e.text)) continue; // the turn card says this, live
      frag.append(el("div", "msg system", e.text)); continue;
    }
    const card = el("div", "card");
    const ttl = el("div", "ttl"); ttl.append(el("span", "", `agent · turn ${e.tick}`));
    card.append(ttl);
    if (notes[e.tick]) { const ul = el("ul", "notes"); notes[e.tick].forEach(n => ul.append(el("li", "", n))); card.append(ul); }
    card.append(el("div", "txt", e.text));
    const diff = (e.diff || []).filter(d => d.status !== "rejected");
    if (diff.length) {
      const det = el("details"), sum = el("summary");
      sum.textContent = `${diff.length} change${diff.length === 1 ? "" : "s"}` + (e.undone ? " (undone)" : "");
      det.append(sum);
      const ul = el("ul");
      for (const d of diff) ul.append(el("li", "", `${paramLabel(d.param)}: ${(+d.from).toFixed(1)} → ${(+d.to).toFixed(1)}${d.status !== "applied" ? " (" + d.status + ")" : ""}`));
      det.append(ul);
      card.append(det);
      if (!e.undone) {
        const cb = el("button", "cmp" + (cmp && cmp.tick === e.tick ? " on" : ""), cmp && cmp.tick === e.tick ? "Hearing before" : "Compare");
        cb.title = "Hear the mix before this turn; click again to come back (playback level stays the same while you compare)";
        cb.onclick = () => (cmp && cmp.tick === e.tick ? endCompare(true) : startCompare(e.tick, diff));
        const u = el("button", "undo", "Undo turn"); u.onclick = () => { endCompare(false); api("undo_turn", { tick: e.tick }).catch(showErr); };
        ttl.append(cb, u);
      }
    }
    frag.append(card);
  }
  if (live) frag.append(liveCard(live, notes[live.tick] || []));
  log.replaceChildren(frag);
  if (atBottom || live) log.scrollTop = log.scrollHeight;
}
function liveCard(tu, notes) {
  const card = el("div", "card live");
  const ttl = el("div", "ttl"); ttl.append(el("span", "", `turn ${tu.tick || ""} · in progress`));
  const stop = el("button", "undo", "Stop");
  stop.title = "Stop the agent: this turn ends with nothing changed, and the rest of a remix is dropped";
  stop.onclick = () => { stop.disabled = true; api("stop_turns", {}).then(() => toast("Stopped")).catch(e => toast(e.message, true)); };
  ttl.append(stop);
  card.append(ttl);
  const steps = el("div", "steps"), k = STAGES.indexOf(tu.stage);
  STAGES.forEach((_, j) => steps.append(el("i", j < k ? "done" : j === k ? "now" : "")));
  card.append(steps);
  const st = el("div", "stage");
  fillStage(st, tu);
  card.append(st);
  liveRef = { tick: tu.tick, stage: tu.stage, notes: notes.length, st };
  if (notes.length) { const ul = el("ul", "notes"); notes.forEach(n => ul.append(el("li", "", n))); card.append(ul); }
  return card;
}
let liveRef = null;
function fillStage(st, tu) {
  st.replaceChildren();
  const since = Math.round(tu.since_s || 0);
  if (tu.stage === "listening") {
    const r = tu.range || analysisRange();
    st.textContent = `Analyzing ${fmtPos(r[0])}–${fmtPos(r[1])} in the background (nothing plays) · ${(tu.listen_s || 0).toFixed(0)} / ${(tu.listen_need_s || r[1] - r[0]).toFixed(0)} s`;
    const b = el("div", "bar"), i = el("i"); i.style.width = `${clamp((tu.listen_s || 0) / Math.max(1, tu.listen_need_s || 1), 0, 1) * 100}%`; b.append(i); st.append(b);
  } else if (tu.stage === "waiting") {
    const aa = ST.auto_agent || {};
    if (aa.state === "starting" || aa.state === "running") st.textContent = `Starting your coding agent (${aa.cli || "agent"}) in the background · ${since} s`;
    else st.textContent = `Sent to your coding agent; waiting for it to pick the turn up · ${since} s`;
    if (aa.state === "unavailable") {
      const h = el("div", "help");
      h.textContent = `The mixer could not start a coding agent${aa.error ? ` (${aa.error})` : ""}. Open Claude Code or Codex and say "keep answering the mixer".`;
      st.append(h);
    } else if ((ST.agent_seen_s ?? 1e9) > 60 && !(aa.state === "starting" || aa.state === "running")) {
      const h = el("div", "help");
      h.textContent = "No coding agent is listening. In Claude Code or Codex, say \"keep answering the mixer\" (it uses the mix tools); this turn starts as soon as it connects.";
      st.append(h);
    }
  } else if (tu.stage === "reading") st.textContent = `Your agent is reading the measurements and working · ${since} s`;
  else if (tu.stage === "applying") st.textContent = `Applying ${tu.changes || ""} changes`;
}
function showAgent() {
  const pill = $("agentPill"), txt = pill.lastElementChild, seen = (ST.agent_seen_s ?? 1e9) < 60;
  const tu = ST.turn && ST.turn.stage && ST.turn.stage !== "done" ? ST.turn : null;
  let cls = seen ? "on" : "", s = seen ? "agent connected" : "no agent";
  if (tu) {
    cls = tu.stage === "waiting" && !seen ? "wait" : "busy";
    s = { listening: "listening…", waiting: seen ? "agent picking up…" : "waiting for agent", reading: `agent working ${Math.round(tu.since_s || 0)} s`, applying: "applying…" }[tu.stage] || "working";
  } else if (ST.agent === "thinking") { cls = seen ? "busy" : "wait"; s = seen ? `agent working ${Math.round(ST.thinking_s || 0)} s` : "waiting for agent"; }
  pill.className = cls; txt.textContent = s;
  pill.title = seen ? "Your coding agent is connected through the mix tools." : "No coding agent has polled in the last minute. In Claude Code or Codex, ask it to use the mix tools (\"mix the tracks in …\" or \"keep answering the mixer\").";
  $("agentState").textContent = s;
}
function ask() {
  const ta = $("prompt"), text = ta.value.trim();
  if (!text) { ta.focus(); return toast("Type what you want changed, then Ask"); }
  ta.value = "";
  api("prompt", { text }).catch(showErr);
}
// A/B in the transport: the raw tracks (summed, level-matched) against the mix. Instant: the engine crossfades.
let rawCmp = false;
function toggleRaw() {
  rawCmp = !rawCmp; paintAB(); paintTransport.pending = Date.now() + 800;
  api("compare", { raw: rawCmp }).catch(e => { rawCmp = false; paintAB(); toast(`A/B: ${e.message}`, true); });
}
function paintAB() {
  $("ab").classList.toggle("on", rawCmp);
  $("ab").textContent = rawCmp ? "A: raw tracks" : "A/B";
}
// Compare on a turn card: the parameters the turn changed go back to their values before it while held, with the
// playback gain frozen so the level does not re-adjust in between.
let cmp = null;
function startCompare(tick, diff) {
  endCompare(true);
  cmp = { tick, diff };
  api("loudness", { hold: true }).catch(() => {});
  for (const d of diff) { const sp = byId[d.param]; if (sp) setParam(sp.i, +d.from, { record: false }); }
  renderChat();
  toast(`Hearing the mix before turn ${tick}`);
}
function endCompare(restore) {
  if (!cmp) return;
  const c = cmp; cmp = null;
  if (restore) for (const d of c.diff) { const sp = byId[d.param]; if (sp) setParam(sp.i, +d.to, { record: false }); }
  api("loudness", { hold: false }).catch(() => {});
  renderChat();
}
// Which part of the song the agent analyzes, said where the user types (with how to change it).
function paintRangeHint() {
  const box = $("rangeHint"), r = analysisRange(), whole = !!ST.analysis_whole;
  const key = `${r[0]}|${r[1]}|${whole}|${clockBars}|${songLen()}`;
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  box.replaceChildren();
  const span = r[1] - r[0];
  if (whole) box.append("Your agent analyzes ", Object.assign(el("b"), { textContent: "the whole song" }), ` (${fmtTime(songLen()).replace(/\.\d$/, "")}).`);
  const rng = clockBars ? `bars ${toBars(r[0]).bar}–${toBars(r[1] - 0.001).bar} (${fmtTime(r[0])}–${fmtTime(r[1])})` : `${fmtTime(r[0])}–${fmtTime(r[1])}`;
  if (!whole) box.append("Your agent analyzes only ", Object.assign(el("b"), { textContent: rng }),
    ` (${Math.round(span)} s of ${fmtTime(songLen()).replace(/\.\d$/, "")}, the teal band on the ruler), not the whole song. Drag in the ruler to choose another part.`);
  const b = el("button", "", whole ? "Only the selection" : "Whole song");
  b.title = whole ? "Analyze the cycle region again" : "Analyze the whole song (each turn takes longer: about 30 s more for a 4-minute, 20-track song)";
  b.onclick = () => api("analysis_range", { whole: !whole }).catch(e => toast(e.message, true));
  box.append(b);
}

// ---- transport -------------------------------------------------------------------------------------------------------
const PLAY_SVG = '<svg viewBox="0 0 16 16"><path d="M4 2l10 6-10 6z"/></svg>', STOP_SVG = '<svg viewBox="0 0 16 16"><path d="M3 3h10v10H3z"/></svg>';
const togglePlay = () => api("transport", { play: !ST.playing }).catch(showErr);
const toggleCycle = () => api("transport", { loop: !ST.looping }).catch(showErr);
const goStart = () => seek(ST.looping && ST.loop_region && ST.loop_region[1] > ST.loop_region[0] ? ST.loop_region[0] : 0);
function paintTransport() {
  const p = $("play");
  if (p.dataset.on !== String(!!ST.playing)) { p.innerHTML = ST.playing ? STOP_SVG : PLAY_SVG; p.dataset.on = String(!!ST.playing); p.classList.toggle("on", !!ST.playing); }
  $("cycle").classList.toggle("on", !!ST.looping);
  $("clock").innerHTML = clockBars ? `${fmtBars(ST.pos || 0)} <small>${fmtTime(ST.pos)}</small>` : `${fmtTime(ST.pos)} <small>/ ${fmtTime(songLen())}</small>`;
  if (ST.compare_raw !== undefined && ST.compare_raw !== rawCmp && Date.now() > (paintTransport.pending || 0)) { rawCmp = !!ST.compare_raw; paintAB(); }
  paintRangeHint();
  const L = ST.loudness;
  const ls = $("loud");
  if (L && document.activeElement !== ls) ls.value = L.target == null ? "" : String(L.target);
  $("loudGain").textContent = !L || L.target == null ? "" : L.state === "measuring" ? "measuring…" : `master ${fmtDb(L.gain_db || 0)} dB`;
  $("loudGain").title = L && L.measured_lufs != null ? `The mix as mixed measures ${L.measured_lufs.toFixed(1)} LUFS over the whole song` : "";
}
function setView(v) {
  view = v;
  $("arrange").hidden = v !== "tracks"; $("mixer").hidden = v !== "mixer";
  $("vTracks").classList.toggle("on", v === "tracks"); $("vMixer").classList.toggle("on", v === "mixer");
  if (v === "mixer") buildMixer(); else { $("mixer").replaceChildren(); for (const k of Object.keys(controls)) controls[k] = controls[k].filter(f => !f.mix); requestAnimationFrame(() => { arrSize(); redrawAll(); }); }
  if (sel.track !== null) document.querySelectorAll(".strip").forEach(h => h.classList.toggle("sel", +h.dataset.t === sel.track));
}

// ---- project title: click for recent projects, double-click to rename -------------------------------------------
function projectName() { return (S && S.name) || (S && S.folder.split(/[\\/]/).filter(Boolean).pop()) || "mix_final_ACTUAL.wav"; }
function paintTitle() {
  $("title").textContent = projectName();
  document.title = `${projectName()} · mix_final_ACTUAL.wav`;
}
function initTitle() {
  const t = $("title");
  paintTitle();
  t.onclick = () => { clearTimeout(t.timer); t.timer = setTimeout(openRecent, 260); };
  t.ondblclick = () => {
    clearTimeout(t.timer); $("recentMenu").hidden = true;
    const inp = el("input"); inp.type = "text"; inp.value = projectName();
    t.replaceChildren(inp); inp.focus(); inp.select();
    let done = false;
    const fin = async ok => {
      if (done) return; done = true;
      const v = inp.value.trim();
      if (ok && v && v !== projectName()) {
        try { await api("project", { name: v }); S.name = v; toast(`Renamed to ${v}`); } catch (e) { toast(`Rename: ${e.message}`, true); }
      }
      paintTitle();
    };
    inp.onkeydown = e => { e.stopPropagation(); if (e.key === "Enter") fin(true); if (e.key === "Escape") fin(false); };
    inp.onblur = () => fin(true);
    inp.onclick = e => e.stopPropagation();
  };
  document.addEventListener("pointerdown", e => { if (!$("recentMenu").hidden && !$("recentMenu").contains(e.target) && e.target !== t) $("recentMenu").hidden = true; });
}
async function openRecent() {
  const m = $("recentMenu"), r = $("title").getBoundingClientRect();
  m.style.left = `${r.left}px`; m.style.top = `${r.bottom + 4}px`;
  m.replaceChildren(el("div", "hd", "Recent projects"));
  m.hidden = false;
  let list = [];
  try { const j = await api("recent"); list = j.projects || j.recent || j || []; } catch (_) {}
  if (!list.length) m.append(el("div", "mi", "No other projects yet"));
  for (const p of list) {
    const it = el("div", "mi");
    const when = p.last_opened ? new Date(p.last_opened * (p.last_opened < 1e12 ? 1000 : 1)).toLocaleString() : "";
    it.append(el("span", "", p.name || p.folder.split(/[\\/]/).pop()), el("small", "", `${p.folder}${when ? " · " + when : ""}`));
    it.title = p.folder;
    if (p.current) it.append(el("small", "", "open now"));
    if (p.exists === false) { it.style.opacity = .45; it.append(el("small", "", "folder not found")); }
    it.onclick = async () => {
      m.hidden = true;
      if (p.current || (S && p.folder === S.folder)) return toast("This project is already open");
      if (p.exists === false) return toast("That folder is gone", true);
      toast(`Opening ${p.name || p.folder}…`);
      try { const o = await api("open", { folder: p.folder }); if (o.url) location.href = o.url; } catch (e) { toast(`Open: ${e.message}`, true); }
    };
    m.append(it);
  }
  const hint = el("div", "hd", "Double-click the title to rename this project");
  m.append(hint);
}

// ---- export ----------------------------------------------------------------------------------------------------------
let exportPolling = false;
async function openExport() {
  $("exportDlg").hidden = false;
  $("exLoud").value = $("loud").value;
  if (!$("exPath").value) {
    try { const d = await api("export_defaults"); $("exPath").value = d.path || ""; } catch (_) { $("exPath").value = ""; }
  }
  syncExt();
  const e = await api("export").catch(() => null);
  if (e && e.running) pollExport();
}
function syncExt() {
  const p = $("exPath"), f = $("exFormat").value;
  if (p.value) p.value = p.value.replace(/\.(wav|flac)$/i, "") + "." + f;
}
async function pollExport() {
  if (exportPolling) return;
  exportPolling = true;
  try {
    for (;;) {
      const e = await api("export");
      $("exBarWrap").hidden = !e.running && !e.progress;
      $("exBar").style.width = `${Math.round(100 * (e.progress || 0))}%`;
      $("exGo").disabled = e.running;
      if (e.running) { $("exState").textContent = `Exporting ${Math.round(100 * (e.progress || 0))}%`; await new Promise(r => setTimeout(r, 400)); continue; }
      if (e.error) $("exState").textContent = `Export failed: ${e.error}`;
      else if (e.file || e.folder) {
        const f = e.file || e.folder;
        $("exState").textContent = `Saved ${f.split(/[\\/]/).pop()}${e.lufs != null ? ` (${e.lufs.toFixed(1)} LUFS)` : ""}`;
        $("exState").title = f;
        $("exReveal").hidden = false; $("exReveal").onclick = () => api("reveal", { path: f }).catch(err => toast(err.message, true));
      }
      break;
    }
  } finally { exportPolling = false; }
}
function initExport() {
  $("exportBtn").onclick = openExport;
  $("exCancel").onclick = () => { $("exportDlg").hidden = true; };
  $("exFormat").onchange = syncExt;
  $("exChoose").onclick = async () => {
    $("exChoose").disabled = true; $("exState").textContent = "Choose where to save in the dialog that opened…";
    try {
      const r = await api("choose_save", { path: $("exPath").value });
      if (r.path) { $("exPath").value = r.path; const m = /\.(wav|flac)$/i.exec(r.path); if (m) $("exFormat").value = m[1].toLowerCase(); syncExt(); }
      $("exState").textContent = r.unavailable ? "No file dialog on this system: type the path." : "";
    } catch (e) { $("exState").textContent = e.message; }
    $("exChoose").disabled = false;
  };
  $("exGo").onclick = async () => {
    const l = $("exLoud").value;
    const body = { path: $("exPath").value.trim(), format: $("exFormat").value, stems: $("exStems").checked };
    if (!body.path) return toast("Choose where to save the mix", true);
    if (l) body.loudness = +l;
    $("exReveal").hidden = true;
    try { await api("export", body); pollExport(); } catch (e) { $("exState").textContent = `Export failed: ${e.message}`; }
  };
}

// ---- keys --------------------------------------------------------------------------------------------------------------
const KEYS = [
  ["Space", "play / stop"], ["Return", "go to the start of the cycle"], ["Home / End", "song start / end"],
  [", .  or  ← →", "playhead back / forward 1 s (Shift: 5 s)"], ["C", "cycle (loop) on / off"], ["Shift+C", "cycle back to the agent's excerpt"],
  ["↑ ↓", "previous / next track"], ["M / S", "mute / solo the selected track"], [`${MOD}+E`, "split the clip at the playhead"],
  ["B", "scissors: click a clip to split it there (B or Esc: back to the pointer)"],
  [`${MOD}+X / ${MOD}+C / ${MOD}+V`, "cut / copy the selected clip; paste at the playhead on the selected track"],
  ["Delete", "delete the selected clip"], [`${MOD}+Z / Shift+${MOD}+Z`, "undo / redo"], ["Q", "snap to the grid on / off"], ["T", "tap the tempo"],
  ["= / -", "zoom in / out"], ["Z", "zoom to fit"],
  ["X", "Tracks / Mixer"], ["I", "inspector on / off"], ["A", "A/B: raw tracks / your mix (level-matched)"], ["/", "type a request to the agent"],
  [`${MOD}+B`, "export"], [`${MOD}+S`, "save now (it also saves every 10 s)"],
  ["Esc", "leave a text field, close a dialog, clear the selection"], ["Alt+click", "reset a control to default; on a strip block: on / off"],
  ["Shift-drag", "fine adjust; on clips: no snapping"], ["Drag a clip's knob", "clip gain (also Alt-drag)"], ["Wheel over clips or the mixer", "scroll left / right (Shift: up / down; over track names: up / down)"],
  [`${MOD}+wheel / pinch`, "zoom the tracks"],
  ["Click the clock", "bars / minutes"], ["Double-click the title", "rename the project"],
];
function showKeys() {
  const l = $("keysList"); l.replaceChildren();
  for (const [k, d] of KEYS) { const a = el("div"); a.append(...k.split(/(\s+\/\s+|\s+or\s+)/).map(x => /^\s/.test(x) ? document.createTextNode(x) : Object.assign(el("kbd"), { textContent: x }))); l.append(a, el("div", "", d)); }
  $("keysDlg").hidden = false;
}
function nudge(d) { seek((ST.pos || 0) + d); }
function onKey(e) {
  const tag = e.target.tagName, typing = tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
  const mod = e.metaKey || e.ctrlKey;
  if (e.key === "Escape") {
    if (!$("exportDlg").hidden || !$("keysDlg").hidden) { $("exportDlg").hidden = $("keysDlg").hidden = true; return; }
    if (typing) { e.target.blur(); return; }
    if (tool === "scissors") { setTool("pointer"); return; }
    sel.clip = null; drawLanes(); refreshClipSection(); return;
  }
  if (typing) return;
  if (!$("exportDlg").hidden || !$("keysDlg").hidden) return;
  const k = e.key.toLowerCase();
  const act = f => { e.preventDefault(); f(); };
  if (mod) {
    if (k === "z") return act(() => (e.shiftKey ? redo() : undo()));
    if (k === "y") return act(redo);
    if (k === "e") return act(splitAtPlayhead);
    if (k === "c") return act(() => copyClip(false));
    if (k === "x") return act(() => copyClip(true));
    if (k === "v") return act(pasteClip);
    if (k === "b") return act(openExport);
    if (k === "s") return act(() => api("save", {}).then(() => toast("Saved")).catch(() => toast("The project saves itself every 10 s")));
    if (k === "=" || k === "+") return act(() => zoomAt(1.5, AR.w / 2));
    if (k === "-") return act(() => zoomAt(1 / 1.5, AR.w / 2));
    return;
  }
  if (e.altKey) return;
  switch (e.key) {
    case " ": return act(togglePlay);
    case "Enter": return act(goStart);
    case "Home": return act(() => seek(0));
    case "End": return act(() => seek(songLen()));
    case ",": case "ArrowLeft": return act(() => nudge(e.shiftKey ? -5 : -1));
    case ".": case "ArrowRight": return act(() => nudge(e.shiftKey ? 5 : 1));
    case "ArrowUp": return act(() => selectTrack(sel.track === null || sel.track <= 0 ? 0 : sel.track - 1));
    case "ArrowDown": return act(() => selectTrack(sel.track === null || sel.track < 0 ? 0 : Math.min(nTracks() - 1, sel.track + 1)));
    case "Delete": case "Backspace": return act(deleteClip);
    case "=": case "+": return act(() => zoomAt(1.5, AR.w / 2));
    case "-": case "_": return act(() => zoomAt(1 / 1.5, AR.w / 2));
    case "/": return act(() => $("prompt").focus());
    case "?": return act(showKeys);
  }
  switch (k) {
    case "c": return act(e.shiftKey ? cycleToExcerpt : toggleCycle);
    case "m": case "s": return act(() => { if (sel.track === null || sel.track < 0) return toast("Select a track first"); const p = P(sel.track, k === "m" ? "mute.on" : "solo.on"); setParam(p.i, V[p.i] > 0.5 ? 0 : 1); });
    case "z": return act(zoomFit);
    case "x": return act(() => setView(view === "tracks" ? "mixer" : "tracks"));
    case "i": return act(() => { $("inspector").hidden = !$("inspector").hidden; requestAnimationFrame(() => { arrSize(); redrawAll(); }); });
    case "a": return act(toggleRaw);
    case "q": return act(() => { SNAP.on = !SNAP.on; saveSnap(); toast(SNAP.on ? `Snap to ${$("gridSel").selectedOptions[0].text}` : "Snap off"); });
    case "t": return act(tapTempo);
    case "b": return act(() => setTool(tool === "scissors" ? "pointer" : "scissors"));
  }
}

// ---- polling ------------------------------------------------------------------------------------------------------------
let lastPos = -1, lastLoopKey = "";
async function poll() {
  if (stopped) return;
  try {
    ST = await api("status");
    paintTransport();
    paintMeters();
    showAgent();
    const loopKey = `${ST.looping}|${(ST.loop_region || []).join(",")}|${(ST.excerpt || []).join(",")}|${songLen()}`;
    if (loopKey !== lastLoopKey) { lastLoopKey = loopKey; if (!AR.fitted) zoomFit(); drawRuler(); drawLanes(); }
    if (ST.pos !== lastPos) {
      lastPos = ST.pos;
      if (ST.playing && view === "tracks") {
        const x = tx(ST.pos);
        if (x > AR.w * 0.92 || x < 0) { AR.x0 = ST.pos - AR.w * 0.08 / AR.pps; clampView(); drawLanes(); drawHbar(); }
      }
      drawRuler(); drawOverlay();
    } else if (ST.turn && ST.turn.stage === "listening") drawOverlay();
    // The live turn card: rebuilt when the turn or its stage changes, its timers patched in place otherwise
    // (rebuilding every second would swap the Stop button out from under the pointer).
    const live = ST.turn && ST.turn.stage && ST.turn.stage !== "done" ? ST.turn : null;
    const key = live ? `${live.tick}|${live.stage}` : "";
    if (key !== poll.key) { poll.key = key; renderChat(); }
    else if (live && liveRef && liveRef.st.isConnected) {
      const t2 = `${Math.floor(live.since_s || 0)}|${Math.floor(live.listen_s || 0)}|${(ST.agent_seen_s ?? 1e9) > 60}|${(ST.auto_agent || {}).state}`;
      if (t2 !== poll.t2) { poll.t2 = t2; fillStage(liveRef.st, live); }
    }
    pollChat(ST.chat_version).catch(() => {});
    if (ST.peaks_ready && PK.some(x => !x)) loadPeaks();
  } catch (e) { showErr(e); return; }
  setTimeout(poll, 50);
}
async function pollValues() {
  if (stopped) return;
  try {
    const nv = await api("values");
    for (const [i, v] of Object.entries(nv)) {
      if (V[i] !== undefined && Math.abs(V[i] - v) <= 1e-6) continue;
      // A read that left before our own change reached the console must not undo it on screen (and the next click
      // would then toggle from the wrong state).
      if (pendingPost.has(+i) || Date.now() - (localSetAt.get(+i) || 0) < 1500) continue;
      V[i] = v; refresh(+i);
    }
  } catch (e) { showErr(e); return; }
  setTimeout(pollValues, 500);
}
let peaksLoading = false;
async function loadPeaks() {
  if (peaksLoading || !S) return;
  peaksLoading = true;
  for (let t = 0; t < nTracks(); ++t) {
    if (PK[t]) continue;
    try {
      const r = await fetch(`/api/peaks?track=${t}&t=${T}`);
      if (r.ok) { const b = new Uint8Array(await r.arrayBuffer()); if (b.length) PK[t] = b; }
    } catch (_) {}
  }
  peaksLoading = false;
  drawLanes();
}
async function reloadSession() {
  S = await api("session");
  indexParams();
  buildHeaders();
  if (view === "mixer") buildMixer();
  buildInspector();
  drawLanes();
}
function indexParams() {
  S.paramsByI = {};
  for (const s of S.params) { byKey[`${s.track}|${s.module}.${s.name}`] = s; byId[s.id] = s; S.paramsByI[s.i] = s; }
}

// ---- first mix wait, init -------------------------------------------------------------------------------------------------
async function waitForLive() {
  const w = $("wait");
  $("skip").onclick = e => { e.target.disabled = true; e.target.textContent = "Opening the mixer..."; api("skip_first_mix", {}).catch(showErr); };
  for (;;) {
    const st = await api("state");
    if (st.phase === "live") { w.hidden = true; return; }
    w.hidden = false;
    const f = st.first_mix;
    $("waitBar").style.width = `${100 * Math.max(0, f.turn - 1) / f.turns}%`;
    $("waitText").textContent = f.turn === 0 ? "Preparing the session..."
      : `Turn ${f.turn} of ${f.turns}` + (st.turn_waiting ? (st.agent_seen_s < 120 ? ": your coding agent is answering." : ": waiting for your coding agent to connect (ask it to use the mix tools).") : ": the console is listening.");
    await new Promise(r => setTimeout(r, 1000));
  }
}
async function init() {
  await waitForLive();
  S = await api("session");
  V = await api("values");
  indexParams();
  ST = await api("status");
  const folder = S.folder.split(/[\\/]/).filter(Boolean).pop() || "";
  $("title").textContent = folder || "mix_final_ACTUAL.wav";
  $("title").title = `${S.folder}\nClick: recent projects. Double-click: rename the project.`;
  document.title = `${folder} · mix_final_ACTUAL.wav`;
  await loadClips();
  buildHeaders();
  initLanes();
  arrSize(); zoomFit();
  loadPeaks();
  $("play").onclick = togglePlay;
  $("cycle").onclick = toggleCycle;
  $("toStart").onclick = goStart;
  $("loud").onchange = () => api("loudness", { target: $("loud").value === "" ? null : +$("loud").value }).catch(e => toast(`Loudness: ${e.message}`, true));
  $("ab").onclick = toggleRaw;
  $("vTracks").onclick = () => setView("tracks");
  $("vMixer").onclick = () => setView("mixer");
  $("helpBtn").onclick = showKeys;
  $("keysClose").onclick = () => { $("keysDlg").hidden = true; };
  $("askBtn").onclick = ask;
  $("prompt").addEventListener("keydown", e => { if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); ask(); } });
  initTempo();
  loadTempo();
  initTitle();
  initExport();
  document.addEventListener("keydown", onKey);
  const sc = el("button", "", "✂"); sc.id = "scissorsBtn"; sc.title = "Scissors: click a clip to split it (B)";
  sc.onclick = () => setTool(tool === "scissors" ? "pointer" : "scissors");
  $("corner").insertBefore(sc, $("corner").querySelector(".spacer"));
  const bb = el("button", "", "Buses ›"); bb.title = "Group buses, effect returns and the master (in the Mixer)";
  bb.onclick = () => { setView("mixer"); requestAnimationFrame(() => { const b = $("busStart"); if (b) $("mixer").scrollLeft = b.offsetLeft - 8; }); };
  $("corner").insertBefore(bb, $("corner").querySelector(".spacer"));
  poll();
  pollValues();
  pollSpectrum();
}
init().catch(showErr);
