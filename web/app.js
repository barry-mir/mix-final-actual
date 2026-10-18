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
function setParam(i, v, { record = true } = {}) {
  const from = V[i];
  if (from === v) return;
  V[i] = v;
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
  add("reverb", "rev", "var(--send)", pv(t, "reverb.return") > -89.9, `${pv(t, "reverb.decay").toFixed(1)} s · ${fmtDb(pv(t, "reverb.return"), true)}`);
  add("echo", "echo", "var(--send)", pv(t, "echo.return") > -89.9, `${pv(t, "echo.time_l").toFixed(0)}/${pv(t, "echo.time_r").toFixed(0)} ms`);
  ["vocals", "drums", "bass", "guitars", "keys"].forEach((b, k) => {
    const on = pon(t, `bus_${b}.on`);
    add(`bus_${b}`, GSHORT[k].toLowerCase(), GCOL[k], on, on ? `${pv(t, `bus_${b}.ratio`).toFixed(1)}:1 · ${pv(t, `bus_${b}.threshold`).toFixed(0)}` : "off", `bus_${b}.on`);
  });
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
  ["buscomp", "glue compressor", ["buscomp"], "buscomp.on"], ["limiter", "limiter", ["limiter", "fader"], "limiter.on"],
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
  } else h.append(el("span", "", "Master"));
  box.append(h);
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
  const mods = {};
  for (const s of S.params) if (s.track === t) (mods[s.module] ||= []).push(s);
  for (const [id, title, modules, powerKey] of t >= 0 ? SECTIONS : MSECTIONS) {
    const sec = el("div", "sec"); sec.dataset.sec = id;
    const h3 = el("h3", "", title);
    if (powerKey && P(t, powerKey)) { const b = withInsp(() => toggle(P(t, powerKey), "", "pw")); h3.append(b); }
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
  const fl = trackLen(t);
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
  const c = E[t][k], fl = trackLen(t), prev = E[t][k - 1], next = E[t][k + 1];
  c.src = clamp(c.src, 0, fl - 0.05);
  c.len = clamp(c.len, 0.05, fl - c.src);
  if (key === "pos") c.pos = clamp(c.pos, prev ? prev.pos + prev.len : 0, next ? next.pos - c.len : 1e9);
  if (next && c.pos + c.len > next.pos) c.len = Math.max(0.05, next.pos - c.pos);
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
  try { const r = await api("edits"); E = S.tracks.map((_, t) => (r.tracks && r.tracks[t] && r.tracks[t].length ? r.tracks[t] : defaultClips(t))); }
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
  const c = clips[k], d = x - c.pos;
  const a = { ...c, len: d, fadeOut: Math.min(c.fadeOut, d / 2), fadeIn: Math.min(c.fadeIn, d / 2) };
  const b = { ...c, pos: x, src: c.src + d, len: c.len - d, fadeIn: 0, fadeOut: Math.min(c.fadeOut, (c.len - d) / 2) };
  a.fadeOut = 0;
  const next = [...clips.slice(0, k), a, b, ...clips.slice(k + 1)];
  sel.clip = k + 1;
  postClips(t, next);
}
function deleteClip() {
  const t = sel.track, k = sel.clip;
  if (t === null || t < 0 || k === null || !E[t][k]) return;
  postClips(t, E[t].filter((_, j) => j !== k));
  sel.clip = null;
  refreshClipSection();
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
function drawRuler() {
  const c = $("ruler"), [g, w, h] = sizeCanvas(c);
  g.clearRect(0, 0, w, h);
  const st = tickStep(), t0 = Math.floor(AR.x0 / st) * st;
  g.font = "10px -apple-system, sans-serif"; g.textBaseline = "top";
  for (let t = t0; tx(t) < w; t += st) {
    const x = Math.round(tx(t)) + .5;
    g.strokeStyle = "#4a515c"; g.beginPath(); g.moveTo(x, 14); g.lineTo(x, h); g.stroke();
    g.fillStyle = "#8d95a0"; g.fillText(st < 1 ? `${Math.floor(t / 60)}:${(t % 60).toFixed(st < 0.5 ? 2 : 1).padStart(st < 0.5 ? 5 : 4, "0")}` : fmtTime(t).replace(/\.0$/, ""), x + 3, 14);
    for (let k = 1; k < 4; ++k) { const xx = Math.round(tx(t + st * k / 4)) + .5; g.strokeStyle = "#343a43"; g.beginPath(); g.moveTo(xx, 22); g.lineTo(xx, h); g.stroke(); }
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
  // the agent's excerpt
  const ex = ST.excerpt || [0, 0];
  if (ex[1] > ex[0]) {
    const xa = tx(ex[0]), xb = tx(ex[1]);
    g.strokeStyle = "#4fd1c5"; g.lineWidth = 1;
    g.beginPath(); g.moveTo(xa + .5, 26); g.lineTo(xa + .5, 23); g.lineTo(xb - .5, 23); g.lineTo(xb - .5, 26); g.stroke();
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
  const st = tickStep(), t0 = Math.floor(AR.x0 / st) * st;
  g.strokeStyle = "#1c2026"; g.lineWidth = 1;
  for (let t = t0; tx(t) < w; t += st) { const x = Math.round(tx(t)) + .5; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke(); }
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
    const peaks = PK[t];
    (E[t] || []).forEach((cl, k) => {
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
      if (x2 - x1 > 50) g.fillText(k ? `${S.tracks[t].name} (${k + 1})` : S.tracks[t].name, Math.max(x1, 0) + 4, top + 7);
      if (Math.abs(cl.gain || 0) >= 0.05 && x2 - x1 > 80) { const s2 = `${fmtDb(cl.gain)} dB`; g.fillText(s2, Math.min(x2, w) - g.measureText(s2).width - 5, top + 7); }
      g.restore();
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
// Lane interaction: move, trim, fades, clip gain; click to place the playhead.
const EDGE = 6;
function hitClip(x, y) {
  const t = Math.floor((y + AR.sy) / AR.laneH);
  if (t < 0 || t >= nTracks()) return { t: -1 };
  const yIn = y + AR.sy - t * AR.laneH;
  const clips = E[t] || [];
  for (let k = 0; k < clips.length; ++k) {
    const c = clips[k], x1 = tx(c.pos), x2 = tx(c.pos + c.len);
    if (x < x1 - EDGE || x > x2 + EDGE) continue;
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
function snapTime(time, t, k, fine) {
  if (fine) return { v: Math.round(time * 1000) / 1000 };
  const cands = [ST.pos || 0, 0, ...(ST.loop_region || [])];
  E.forEach((cl, tt) => cl.forEach((c, kk) => { if (tt === t && kk === k) return; cands.push(c.pos, c.pos + c.len); }));
  let best = null, bd = 8 / AR.pps;
  for (const c of cands) if (Math.abs(c - time) < bd) { bd = Math.abs(c - time); best = c; }
  if (best !== null) return { v: best, snapped: true };
  return { v: Math.round(time * 100) / 100 };
}
function initLanes() {
  const c = $("overlay");
  const cursorFor = z => ({ fadeIn: "nwse-resize", fadeOut: "nesw-resize", trimL: "ew-resize", trimR: "ew-resize", move: "grab" })[z] || "text";
  c.onpointermove = ev => { if (c.dragging) return; const r = c.getBoundingClientRect(); const h = hitClip(ev.clientX - r.left, ev.clientY - r.top); c.style.cursor = ev.altKey && h.zone === "move" ? "ns-resize" : cursorFor(h.zone); };
  c.onpointerdown = ev => {
    if (ev.button !== 0) return;
    const r = c.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
    const h = hitClip(x, y);
    if (h.t < 0) return;
    if (h.k < 0) { sel.clip = null; selectTrack(h.t, { keepClip: false }); seek(xt(x)); return; }
    selectTrack(h.t, { clip: h.k });
    const t = h.t, k = h.k, before = E[t].map(x2 => ({ ...x2 })), c0 = { ...E[t][k] };
    const zone = ev.altKey && h.zone === "move" ? "gain" : h.zone;
    const prev = E[t][k - 1], next = E[t][k + 1], fl = trackLen(t);
    const lo = prev ? prev.pos + prev.len : 0, hi = next ? next.pos : Infinity;
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
        cl.pos = clamp(p, lo, hi - c0.len);
        showTip(m, `${fmtTime(cl.pos)}`);
      } else if (zone === "trimL") {
        const s1 = snapTime(c0.pos + dt, t, k, fine); if (s1.snapped) AR.snapX = tx(s1.v);
        let d = s1.v - c0.pos;
        d = clamp(d, Math.max(-c0.src, lo - c0.pos), c0.len - 0.05);
        cl.pos = c0.pos + d; cl.src = c0.src + d; cl.len = c0.len - d;
        showTip(m, `start ${fmtTime(cl.pos)} · ${cl.len.toFixed(2)} s`);
      } else if (zone === "trimR") {
        const s1 = snapTime(c0.pos + c0.len + dt, t, k, fine); if (s1.snapped) AR.snapX = tx(s1.v);
        cl.len = clamp(s1.v - c0.pos, 0.05, Math.min(fl - c0.src, hi - c0.pos));
        showTip(m, `end ${fmtTime(cl.pos + cl.len)} · ${cl.len.toFixed(2)} s`);
      } else if (zone === "fadeIn") {
        cl.fadeIn = clamp(c0.fadeIn + dt, 0, cl.len / 2); showTip(m, `fade in ${cl.fadeIn.toFixed(2)} s`);
      } else if (zone === "fadeOut") {
        cl.fadeOut = clamp(c0.fadeOut - dt, 0, cl.len / 2); showTip(m, `fade out ${cl.fadeOut.toFixed(2)} s`);
      } else if (zone === "gain") {
        cl.gain = clamp(Math.round((c0.gain - dy * (fine ? 0.02 : 0.1)) * 10) / 10, -24, 12); showTip(m, `clip gain ${fmtDb(cl.gain)} dB`);
      }
      cl.fadeIn = Math.min(cl.fadeIn, cl.len / 2); cl.fadeOut = Math.min(cl.fadeOut, cl.len / 2);
      E[t][k] = cl;
      drawLanes(); drawOverlay();
    };
    c.onpointerup = () => {
      c.onpointermove = null; c.dragging = false; tip.hidden = true; AR.snapX = undefined; drawOverlay();
      c.onpointermove = initLanes.hover;
      if (moved) postClips(t, E[t], { before });
      else refreshClipSection();
    };
  };
  initLanes.hover = c.onpointermove;
  c.ondblclick = ev => { const r = c.getBoundingClientRect(); const h = hitClip(ev.clientX - r.left, ev.clientY - r.top); if (h.k >= 0) selectTrack(h.t, { clip: h.k, section: "clip" }); };
  const wheel = ev => {
    ev.preventDefault();
    const r = $("lanesWrap").getBoundingClientRect();
    if (ev.ctrlKey || ev.metaKey) { zoomAt(Math.exp(-ev.deltaY * 0.01), ev.clientX - r.left); return; }
    let dx = ev.deltaX, dy = ev.deltaY;
    if (ev.shiftKey && !dx) { dx = dy; dy = 0; }
    if (dx) AR.fitted = false;
    AR.x0 += dx / AR.pps; AR.sy += dy;
    clampView(); redrawAll();
  };
  $("lanesWrap").addEventListener("wheel", wheel, { passive: false });
  $("headers").addEventListener("wheel", ev => { ev.preventDefault(); AR.sy += ev.deltaY; clampView(); redrawAll(); }, { passive: false });
  $("rulerWrap").addEventListener("wheel", wheel, { passive: false });
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
  box.append(strip(-1));
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
  if (!master) { const ms = el("div", "ms"); ms.append(toggle(P(t, "mute.on"), "M", "m"), toggle(P(t, "solo.on"), "S", "s")); e.append(ms); }
  e.onpointerdown = () => selectTrack(t);
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
    const strips = $("mixer").children;
    for (let t = 0; t < strips.length; ++t) {
      const s = strips[t], ms = s.meters || [];
      if (+s.dataset.t < 0) {
        const mm = ST.master || [0, 0];
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
let chatVersion = -1, chat = [], lastTurn = null, abState = "B";
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
  const turns = chat.filter(e => e.role === "agent" && !e.undone && (e.diff || []).some(d => !["rejected", "superseded", "unchanged"].includes(d.status)));
  const lt = turns.length ? turns[turns.length - 1] : null;
  if (!lt && lastTurn) { lastTurn = null; abState = "B"; $("ab").classList.remove("on"); $("ab").textContent = "A/B"; }
  if (lt && (!lastTurn || lt.tick !== lastTurn.tick)) { lastTurn = { tick: lt.tick, diff: lt.diff.filter(d => d.status !== "rejected") }; abState = "B"; $("ab").classList.remove("on"); $("ab").textContent = "A/B"; }
  $("ab").disabled = !lastTurn;
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
      if (!e.undone) { const u = el("button", "undo", "Undo turn"); u.onclick = () => api("undo_turn", { tick: e.tick }).catch(showErr); ttl.append(u); }
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
    st.textContent = `Listening to the loop so the measurements hear the current mix · ${(tu.listen_s || 0).toFixed(0)} / ${(tu.listen_need_s || 0).toFixed(0)} s`;
    const b = el("div", "bar"), i = el("i"); i.style.width = `${clamp((tu.listen_s || 0) / Math.max(1, tu.listen_need_s || 1), 0, 1) * 100}%`; b.append(i); st.append(b);
  } else if (tu.stage === "waiting") {
    st.textContent = `Sent to your coding agent; waiting for it to pick the turn up · ${since} s`;
    if ((ST.agent_seen_s ?? 1e9) > 60) {
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
function remix() {
  const ta = $("prompt"), goal = ta.value.trim();
  ta.value = "";
  api("mix", goal ? { turns: 5, goal } : { turns: 5 }).then(() => toast(goal ? `Remix ×5 with the goal: ${goal}` : "Remix ×5 started")).catch(showErr);
}
function setAB(which) {
  if (!lastTurn) return;
  for (const d of lastTurn.diff) { const s = byId[d.param]; if (s) setParam(s.i, which === "A" ? +d.from : +d.to, { record: false }); }
  abState = which;
  $("ab").classList.toggle("on", which === "A");
  $("ab").textContent = which === "A" ? "A (before)" : "A/B";
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
  $("clock").innerHTML = `${fmtTime(ST.pos)} <small>/ ${fmtTime(songLen())}</small>`;
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
  ["Delete", "delete the selected clip"], [`${MOD}+Z / Shift+${MOD}+Z`, "undo / redo"], ["= / -", "zoom in / out"], ["Z", "zoom to fit"],
  ["X", "Tracks / Mixer"], ["I", "inspector on / off"], ["A", "A/B: before / after the agent's last turn"], ["/", "type a request to the agent"],
  [`${MOD}+Enter`, "(in the request box) Remix ×5 with this goal"], [`${MOD}+B`, "export"], [`${MOD}+S`, "save now (it also saves every 10 s)"],
  ["Esc", "leave a text field, close a dialog, clear the selection"], ["Alt+click", "reset a control to default; on a strip block: on / off"],
  ["Shift-drag", "fine adjust; on clips: no snapping"], ["Alt-drag a clip", "clip gain"], [`${MOD}+wheel / pinch`, "zoom the tracks"],
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
    case "a": return act(() => setAB(abState === "B" ? "A" : "B"));
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
    }
    // The live turn card: rebuilt when the turn or its stage changes, its timers patched in place otherwise
    // (rebuilding every second would swap the Stop button out from under the pointer).
    const live = ST.turn && ST.turn.stage && ST.turn.stage !== "done" ? ST.turn : null;
    const key = live ? `${live.tick}|${live.stage}` : "";
    if (key !== poll.key) { poll.key = key; renderChat(); }
    else if (live && liveRef && liveRef.st.isConnected) {
      const t2 = `${Math.floor(live.since_s || 0)}|${Math.floor(live.listen_s || 0)}|${(ST.agent_seen_s ?? 1e9) > 60}`;
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
    for (const [i, v] of Object.entries(nv)) if (V[i] === undefined || Math.abs(V[i] - v) > 1e-6) { if (pendingPost.has(+i)) continue; V[i] = v; refresh(+i); }
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
  $("title").title = `mix_final_ACTUAL.wav · ${S.folder}`;
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
  $("ab").disabled = true;
  $("ab").onclick = () => setAB(abState === "B" ? "A" : "B");
  $("vTracks").onclick = () => setView("tracks");
  $("vMixer").onclick = () => setView("mixer");
  $("helpBtn").onclick = showKeys;
  $("keysClose").onclick = () => { $("keysDlg").hidden = true; };
  $("askBtn").onclick = ask;
  $("remixBtn").onclick = remix;
  $("prompt").addEventListener("keydown", e => {
    if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) { e.preventDefault(); remix(); }
    else if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); ask(); }
  });
  initExport();
  document.addEventListener("keydown", onKey);
  poll();
  pollValues();
}
init().catch(showErr);
