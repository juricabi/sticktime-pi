#!/usr/bin/env node
// Reference frame buffers for host/tests/test_lcd_golden.c: replays each call file through the
// web emulator's own lcd (sticktime/web/src/engine.js on fengari, called as Lua calls the API)
// with a stub font (fixed advance, no glyph masks), on a 480x272 (std) and a 640x480 (lrg) screen.
// See README.md for the call file format and how to run it.
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const opts = { engine: path.resolve(ROOT, '..', 'sticktime', 'web', 'src', 'engine.js'),
  out: path.join(ROOT, 'build', 'golden'), calls: path.join(__dirname, 'calls'), fengari: null };
for (let i = 2; i < process.argv.length; i++) {
  const m = /^--(engine|out|calls|fengari)$/.exec(process.argv[i]);
  if (!m || i + 1 >= process.argv.length) {
    console.error('usage: node ref.js [--engine engine.js] [--out dir] [--calls dir] [--fengari node_modules/fengari]');
    process.exit(2);
  }
  opts[m[1]] = path.resolve(process.argv[++i]);
}

let fengari;
try { fengari = require(opts.fengari || 'fengari'); } catch (e) {
  console.error('fengari not found: npm install --prefix build/golden-node fengari@0.1.4, then run with ' +
    'NODE_PATH=build/golden-node/node_modules (or pass --fengari)');
  process.exit(2);
}
const EdgeTX = require(opts.engine);
const { lua, to_luastring, to_jsstring } = fengari;

const SCREENS = [{ w: 480, h: 272, fonts: 'std' }, { w: 640, h: 480, fonts: 'lrg' }];
const DEFAULT_ADV = 7.3;

// engine.js ColorFonts with the glyphs replaced: every code point advances by adv (a float, as
// the C side stores it) and draws nothing. measure() records the strings drawStr really drew.
class StubFonts extends EdgeTX.ColorFonts {
  constructor(adv) { super(); this.adv = Math.fround(adv); this.log = []; }
  glyph() { return { adv: this.adv, w: 0, h: 0, ox: 0, oy: 0, a: null }; }
  measure(text, px, bold) { this.log.push(text); return super.measure(text, px, bold); }
}

function fnv1a(buf) {
  let h = 0x811c9dc5;
  for (const b of buf) { h ^= b; h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, '0');
}

// one call per line: name, then integers (decimal or 0x hex, int32 or uint32) and "strings"
// (escapes \\ \" \n \r \t \xHH); '#' starts a comment line; 'adv <px>' sets the stub advance
// (first line only), 'snap' records the frame buffer at that point.
function parse(text, file) {
  const calls = [];
  let adv = DEFAULT_ADV;
  text.split('\n').forEach((raw, i) => {
    const line = raw.replace(/\r$/, '');
    const no = i + 1;
    const fail = (msg) => { throw new Error(`${file}:${no}: ${msg}`); };
    let p = 0;
    const toks = [];
    while (p < line.length) {
      const c = line[p];
      if (c === ' ' || c === '\t') { p++; continue; }
      if (c === '#' && !toks.length) break;
      if (c === '"') {
        const bytes = [];
        p++;
        for (;;) {
          if (p >= line.length) fail('unterminated string');
          const d = line[p++];
          if (d === '"') break;
          if (d !== '\\') { for (const b of Buffer.from(d, 'utf8')) bytes.push(b); continue; }
          const e = line[p++];
          if (e === 'x') {
            const hx = line.substr(p, 2);
            if (!/^[0-9a-fA-F]{2}$/.test(hx)) fail('bad \\x escape');
            bytes.push(parseInt(hx, 16)); p += 2;
          } else if (e === 'n') bytes.push(10); else if (e === 'r') bytes.push(13); else if (e === 't') bytes.push(9);
          else if (e === '\\' || e === '"') bytes.push(e.charCodeAt(0)); else fail('bad escape');
        }
        toks.push(Uint8Array.from(bytes));
        continue;
      }
      let q = p;
      while (q < line.length && line[q] !== ' ' && line[q] !== '\t') q++;
      toks.push(line.slice(p, q));
      p = q;
    }
    if (!toks.length) return;
    const name = toks[0];
    if (typeof name !== 'string') fail('a call starts with a name');
    if (name === 'adv') {
      if (calls.length) fail('adv must come before the calls');
      adv = Number(toks[1]);
      if (!(adv >= 0)) fail('bad adv');
      return;
    }
    const args = toks.slice(1).map((t) => {
      if (typeof t !== 'string') return t;
      if (!/^-?(0x[0-9a-fA-F]+|[0-9]+)$/.test(t)) fail(`bad number ${t}`);
      const neg = t[0] === '-';
      const v = Number(neg ? t.slice(1) : t) * (neg ? -1 : 1);
      if (v < -2147483648 || v > 0xFFFFFFFF) fail(`${t} does not fit 32 bits`);
      return v | 0;
    });
    calls.push({ no, name, args });
  });
  return { calls, adv };
}

function callLcd(L, name, args) {
  const top = lua.lua_gettop(L);
  lua.lua_getglobal(L, to_luastring('lcd'));
  lua.lua_getfield(L, -1, to_luastring(name));
  if (lua.lua_type(L, -1) !== lua.LUA_TFUNCTION) throw new Error('no function lcd.' + name);
  for (const a of args) {
    if (a instanceof Uint8Array) lua.lua_pushlstring(L, a, a.length); else lua.lua_pushinteger(L, a);
  }
  if (lua.lua_pcall(L, args.length, lua.LUA_MULTRET, 0) !== lua.LUA_OK) {
    const msg = to_jsstring(lua.lua_tostring(L, -1));
    lua.lua_settop(L, top);
    throw new Error(`lcd.${name}: ${msg}`);
  }
  const res = [];
  for (let i = top + 2; i <= lua.lua_gettop(L); i++) res.push(lua.lua_tonumber(L, i));
  lua.lua_settop(L, top);
  return res;
}

function replay(calls, adv, scr) {
  const fonts = new StubFonts(adv);
  const eng = new EdgeTX.Engine(fengari, { id: 'golden', w: scr.w, h: scr.h, color: true, fonts: scr.fonts, cpu: 'H7' },
    { colorFonts: fonts });
  if (!eng.load('return {}', 'golden.lua', {})) throw new Error('engine: ' + JSON.stringify(eng.error));
  const frames = [], results = [];
  const snap = () => {
    const b = Buffer.alloc(eng.fb.length * 2);
    for (let i = 0; i < eng.fb.length; i++) b.writeUInt16LE(eng.fb[i], i * 2);
    frames.push(b);
  };
  for (const c of calls) {
    if (c.name === 'snap') { snap(); continue; }
    fonts.log.length = 0;
    const r = callLcd(eng.L, c.name, c.args);
    if (c.name === 'sizeText') results.push(`L${c.no} sizeText ${r[0]} ${r[1]}`);
    else if (c.name === 'drawText' || c.name === 'drawNumber' || c.name === 'drawTimer') {
      if (fonts.log.length !== 1) throw new Error(`line ${c.no}: expected one measured string`);
      const hex = Buffer.from(fonts.log[0], 'utf8').toString('hex');
      results.push(`L${c.no} ${c.name} ${hex || '-'}`);
    }
  }
  snap();
  return { frames, results };
}

fs.mkdirSync(opts.out, { recursive: true });
const files = fs.readdirSync(opts.calls).filter((f) => f.endsWith('.txt')).sort();
const index = [];
let total = 0;
for (const f of files) {
  const raw = fs.readFileSync(path.join(opts.calls, f));
  const name = f.slice(0, -4);
  const { calls, adv } = parse(raw.toString('utf8'), f);
  for (const scr of SCREENS) {
    const t0 = Date.now();
    const { frames, results } = replay(calls, adv, scr);
    const base = path.join(opts.out, `${name}.${scr.w}x${scr.h}`);
    fs.writeFileSync(base + '.bin', Buffer.concat(frames));
    fs.writeFileSync(base + '.txt', results.length ? results.join('\n') + '\n' : '');
    console.log(`${name} ${scr.w}x${scr.h}: ${calls.length} calls, ${frames.length} frames, ${Date.now() - t0} ms`);
  }
  index.push(`${name} ${fnv1a(raw)}`);
  total += calls.length;
}
fs.writeFileSync(path.join(opts.out, 'index.txt'),
  `# written by host/tests/golden/ref.js from ${path.relative(ROOT, opts.engine).replace(/\\/g, '/')} (engine.js ${fnv1a(fs.readFileSync(opts.engine))})\n` +
  index.join('\n') + '\n');
console.log(`${files.length} call files, ${total} calls -> ${opts.out}`);
