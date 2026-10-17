/* StickTime Pi: StickTime's emulator page (simulator.html), served by the Pi at /editor/ with this
   script put before </head> (tools/patch_editor.py), so it runs before the page's own scripts.

   The track editor's "radio SD card" is this Pi's SD folder, through /api/sd/ (docs/WEB_API.md):
   window.showDirectoryPicker() answers at once with a directory handle named "StickTime Pi", so
   Connect radio, Send to radio, My tracks, the gallery's Update and Install all work unchanged.
   Install takes the release the Pi's updater staged at /editor/sdcard/; it hashes files with
   crypto.subtle, which an http:// page doesn't have, so SHA-256 is filled in here when missing.
   The page connects by itself and opens the view the address asks for: #tracks (the default),
   #open, #mine, #install or #emulator. Radio wording in its messages is put in the Pi's words.

   Rules: everything inside this function (the page declares its own globals with const: a clash
   would be a SyntaxError in its scripts), plain ES2018, no dependencies. */
(() => {
  'use strict';
  const API = '/api/sd/', ROOT_NAME = 'StickTime Pi', MUT = { 'X-StickTime': '1' };
  const BY_STATUS = { 400: 'TypeError', 403: 'NoModificationAllowedError', 404: 'NotFoundError', 409: 'TypeMismatchError', 413: 'QuotaExceededError', 507: 'QuotaExceededError' };
  const err = (name, msg) => (name === 'TypeError' ? new TypeError(msg) : new DOMException(msg, name));
  const url = (path, q) => API + path.split('/').filter(Boolean).map(encodeURIComponent).join('/') + (q ? '?' + q : '');
  const join = (dir, name) => (dir ? dir + '/' + name : name);
  const wait = (ms) => new Promise((res) => setTimeout(res, ms));

  // ---------------------------------------------------------------- SHA-256 for Install (http pages have no crypto.subtle)
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  function sha256(bytes) {
    const n = bytes.length, total = ((n + 9 + 63) >>> 6) << 6;
    const m = new Uint8Array(total);
    m.set(bytes);
    m[n] = 0x80;
    const dv = new DataView(m.buffer);
    dv.setUint32(total - 8, Math.floor(n / 0x20000000));      // the length in bits, 64-bit big-endian
    dv.setUint32(total - 4, (n << 3) >>> 0);
    const H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const W = new Uint32Array(64);
    for (let off = 0; off < total; off += 64) {
      for (let i = 0; i < 16; i++) W[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const x = W[i - 15], y = W[i - 2];
        const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
        const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
        W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
      }
      let a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
      for (let i = 0; i < 64; i++) {
        const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
        const t1 = (h + S1 + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
        const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
        const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    const out = new ArrayBuffer(32), o = new DataView(out);
    for (let i = 0; i < 8; i++) o.setUint32(i * 4, H[i]);
    return out;
  }
  // only where the browser has none (an https or localhost page keeps the real one)
  if (!(window.crypto && window.crypto.subtle && typeof window.crypto.subtle.digest === 'function')) {
    const subtle = Object.freeze({
      digest(alg, data) {
        return new Promise((res, rej) => {
          const name = String(typeof alg === 'string' ? alg : (alg && alg.name) || '').toUpperCase();
          if (name !== 'SHA-256') { rej(new DOMException('Only SHA-256 is here (the StickTime Pi\'s page isn\'t https).', 'NotSupportedError')); return; }
          let bytes;
          if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
          else if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
          else { rej(new TypeError('digest() takes an ArrayBuffer or a typed array.')); return; }
          res(sha256(bytes));
        });
      },
    });
    try {
      if (window.crypto) Object.defineProperty(window.crypto, 'subtle', { value: subtle, configurable: true });
      else Object.defineProperty(window, 'crypto', { value: { subtle }, configurable: true });
    } catch (e) { /* a browser that won't: Install then says what failed */ }
  }

  // ---------------------------------------------------------------- the SD folder as File System Access handles
  async function call(method, path, q, body) {
    for (let tries = 0; ; tries++) {
      let r;
      try {
        r = await fetch(url(path, q), { method, body, cache: 'no-store', credentials: 'same-origin', headers: method === 'GET' ? {} : MUT });
      } catch (e) {     // the Pi is off or out of reach: NotAllowedError, so the editor offers Reconnect (NotFoundError would read as "gone already")
        throw err('NotAllowedError', 'the StickTime Pi doesn\'t answer: is it on, and this device on its WiFi?');
      }
      if (r.ok) return r;
      if (r.status === 503 && tries < 3) { await wait(400 * (tries + 1)); continue; }   // too many clients at once: the request wasn't run
      let j = null;
      try { j = await r.json(); } catch (e) { /* no JSON body */ }
      throw err((j && typeof j.error === 'string' && j.error) || BY_STATUS[r.status] || 'NotReadableError', (j && typeof j.message === 'string' && j.message) || 'HTTP ' + r.status);
    }
  }
  function validName(name) {                    // as Chrome: TypeError for '', '.', '..', a separator
    name = String(name);
    if (!name || name === '.' || name === '..' || /[/\\\0]/.test(name)) throw new TypeError('Name is not allowed.');
    return name;
  }

  const PATH = new WeakMap();                   // handle -> path from the SD root ('' = root): not cloneable on purpose
  class Handle {
    constructor(kind, name, path) {
      Object.defineProperty(this, 'kind', { value: kind, enumerable: true });
      Object.defineProperty(this, 'name', { value: name, enumerable: true });
      PATH.set(this, path);
    }
    async isSameEntry(o) { return o instanceof Handle && o.kind === this.kind && PATH.get(o).toLowerCase() === PATH.get(this).toLowerCase(); }
    async queryPermission() { return 'granted'; }
    async requestPermission() { return 'granted'; }
  }
  class Dir extends Handle {
    constructor(name, path) { super('directory', name, path); }
    getDirectoryHandle(name, o) { return lookup(this, name, 'directory', !!(o && o.create)); }
    getFileHandle(name, o) { return lookup(this, name, 'file', !!(o && o.create)); }
    async removeEntry(name, o) {
      await call('DELETE', join(PATH.get(this), validName(name)), o && o.recursive ? 'recursive=1' : '');
    }
    async resolve(h) {
      if (!(h instanceof Handle)) return null;
      const a = PATH.get(this).toLowerCase(), b = PATH.get(h);
      if (b.toLowerCase() === a) return [];
      if (!a) return b.split('/');
      return b.toLowerCase().startsWith(a + '/') ? b.slice(a.length + 1).split('/') : null;
    }
    async *entries() {
      const r = await call('GET', PATH.get(this), 'list');
      for (const e of (await r.json()).entries || []) {
        const p = join(PATH.get(this), e.name);
        yield [e.name, e.kind === 'directory' ? new Dir(e.name, p) : new FileH(e.name, p)];
      }
    }
    async *values() { for await (const [, h] of this.entries()) yield h; }
    async *keys() { for await (const [k] of this.entries()) yield k; }
    [Symbol.asyncIterator]() { return this.entries(); }
  }
  // getFileHandle / getDirectoryHandle: NotFoundError and TypeMismatchError as Chrome has them (the editor's child() relies on them)
  async function lookup(dir, name, kind, create) {
    const path = join(PATH.get(dir), validName(name));
    let st = null;
    try { st = await (await call('GET', path, 'stat')).json(); } catch (e) { if (e.name !== 'NotFoundError' || !create) throw e; }
    if (st && st.kind !== kind) throw err('TypeMismatchError', name + ' is a ' + (st.kind === 'directory' ? 'folder' : 'file') + ' on the StickTime Pi.');
    if (!st) await call('POST', path, 'create=' + kind);
    return kind === 'file' ? new FileH(name, path) : new Dir(name, path);
  }
  class FileH extends Handle {
    constructor(name, path) { super('file', name, path); }
    async getFile() {
      const r = await call('GET', PATH.get(this));
      const lm = +r.headers.get('X-Mtime-Ms') || Date.parse(r.headers.get('Last-Modified') || '') || Date.now();
      return new File([await r.arrayBuffer()], this.name, { lastModified: lm });   // a real File: size, text(), arrayBuffer(), slice()
    }
    async createWritable(o) {
      const keep = !!(o && o.keepExistingData);
      return new Writable(this, keep ? new Uint8Array(await (await this.getFile()).arrayBuffer()) : new Uint8Array(0));
    }
  }
  // written in memory and sent whole on close(): one PUT, which the Pi writes to a temp file, fsyncs and
  // renames (as Chrome's swap file: a Pi unplugged midway keeps the old file whole); abort() sends nothing
  async function bytes(c) {
    if (typeof c === 'string') return new TextEncoder().encode(c);
    if (c instanceof Blob) return new Uint8Array(await c.arrayBuffer());
    if (c instanceof ArrayBuffer) return new Uint8Array(c);
    if (ArrayBuffer.isView(c)) return new Uint8Array(c.buffer, c.byteOffset, c.byteLength);
    throw new TypeError('write() takes a string, a Blob, an ArrayBuffer or a typed array.');
  }
  class Writable {
    constructor(h, data) { this._h = h; this._d = data; this._n = data.length; this._pos = 0; this._open = true; }
    _size(n) { if (n > this._d.length) { const d = new Uint8Array(Math.max(n, this._d.length * 2)); d.set(this._d.subarray(0, this._n)); this._d = d; } }
    async write(c) {
      if (!this._open) throw new TypeError('The stream is closed.');
      if (c && typeof c === 'object' && typeof c.type === 'string' && !(c instanceof Blob)) {
        if (c.type === 'seek') return this.seek(c.position);
        if (c.type === 'truncate') return this.truncate(c.size);
        if (c.type !== 'write' || c.data === undefined) throw new TypeError('Bad write command.');
        if (c.position !== undefined && c.position !== null) this._pos = Math.max(0, +c.position || 0);
        c = c.data;
      }
      const b = await bytes(c);
      this._size(this._pos + b.length);          // past the end: the gap stays zeros (as the spec says)
      this._d.set(b, this._pos);
      this._pos += b.length;
      this._n = Math.max(this._n, this._pos);
    }
    async seek(p) { if (!this._open) throw new TypeError('The stream is closed.'); this._pos = Math.max(0, +p || 0); }
    async truncate(s) {
      if (!this._open) throw new TypeError('The stream is closed.');
      s = Math.max(0, +s || 0);
      this._size(s);
      if (s > this._n) this._d.fill(0, this._n, s);
      this._n = s;
      if (this._pos > s) this._pos = s;
    }
    async close() {
      if (!this._open) throw new TypeError('The stream is closed.');
      this._open = false;
      await call('PUT', PATH.get(this._h), '', this._d.slice(0, this._n));
    }
    async abort() { this._open = false; }
    get locked() { return false; }
  }

  const root = new Dir(ROOT_NAME, '');
  // always ours, native or not; never rejects (a SecurityError or TypeError would switch the radio off for the page)
  Object.defineProperty(window, 'showDirectoryPicker', { configurable: true, writable: true, value: async function showDirectoryPicker() { return root; } });

  // ---------------------------------------------------------------- the radio's words, in the Pi's
  // whole texts first, then phrases; each result is free of every phrase (so a second pass changes nothing)
  const WHOLE = [
    ['Eject the drive before you unplug the radio.', 'This page\'s "radio" is the StickTime Pi: its SD folder holds the game and your tracks c1 to c8.'],
    [/^Plug the radio in by USB.*$/, 'Connect radio links this page to the StickTime Pi\'s SD folder (it does so by itself when the page opens).'],
    [/^This doesn't look like the radio's SD card: no SCRIPTS folder\..*$/,
      'The StickTime Pi\'s SD folder has no SCRIPTS folder, so the game isn\'t on it: install it on the settings page (Updates), or copy it into sticktime/sd/SCRIPTS/TOOLS/ on the SD card from a PC. Then Connect radio.'],
  ];
  const PHRASES = [
    [/ ?On the radio: restart StickTime, the tracks? (is|are) in the Track list\./, (m, v) => ' The game on the Pi has ' + (v === 'is' ? 'it' : 'them') + ' in its track list once it is back in its menus.'],
    [/ ?(On the radio: restart StickTime|Restart StickTime on the radio)\./, ' The game on the Pi picks it up once it is back in its menus.'],
    [/restart StickTime on it\./, 'the game on the Pi picks it up once it is back in its menus.'],
    [/ ?Eject the drive before you unplug the radio\./, ''],
    [/The radio's SD card isn't there any more: plug the radio in \(USB Storage\) and Reconnect, or Connect radio to pick it again\./,
      'The StickTime Pi doesn\'t answer: is it on, and this device on its WiFi? Then Reconnect radio.'],
    [/Couldn't get the game's files from the site: check the connection and try again\./,
      'No StickTime release is staged on this Pi: on its settings page, Updates → Check now stages the latest one; then Read again.'],
    [/ from this site, onto /, ' (staged on this Pi) onto '],
    [/the radio's SD card over USB: connect it, /, 'the StickTime Pi\'s SD folder: '],
    [/\b(newer|installed) on the card\b/g, '$1 on the Pi'],
    [/ on the card\)/g, ' on the Pi)'],
    [/than this site's /, 'than the one staged on this Pi, '],
    [/The radio's SD card: /, 'The StickTime Pi: '],
    [/the radio's SD card/g, 'the StickTime Pi\'s SD folder'],
    [/the radio's save/g, 'the Pi\'s save'],
    [/\b(on|from|to|onto) the radio\b/g, '$1 the Pi'],
    [/\bThe radio has\b/g, 'The Pi has'],
    [/\band the radio\b/g, 'and the Pi'],
  ];
  function reword(t) {
    for (const [from, to] of WHOLE) if (typeof from === 'string' ? t === from : from.test(t)) return to;
    for (const [from, to] of PHRASES) t = t.replace(from, to);
    return t;
  }
  // window.confirm too: the editor asks "Slot c1 on the radio holds..."
  const confirm0 = window.confirm;
  window.confirm = function confirm(msg) { return confirm0.call(window, reword(String(msg === undefined ? '' : msg))); };
  const SAID = ['edStatus', 'edMineStatus', 'edGalStatus', 'rpStatus', 'rpConnStatus', 'edRadioNote', 'rpInstWait', 'edInstallHead', 'edSyncText', 'edEditions', 'rpTitle'];
  function fix(el) {
    const w = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    for (let n = w.nextNode(); n; n = w.nextNode()) {
      const t = reword(n.nodeValue);
      if (t !== n.nodeValue) n.nodeValue = t;
    }
  }

  // ---------------------------------------------------------------- the page: words, the view, connecting
  const $ = (id) => document.getElementById(id);
  const ready = () => { const s = window.sim; return !!(s && s.engine && s.editor); };
  const radio = () => (window.sim && window.sim.editor && typeof window.sim.editor.radio === 'function' ? window.sim.editor.radio() : null);
  // each link makes its whole view, whatever the last one left open (the page's buttons toggle)
  function view() {
    const v = (location.hash || '#tracks').slice(1).toLowerCase();
    const click = (id) => { const b = $(id); if (b) b.click(); };
    const open = (panel) => { const p = $(panel); return !!p && !p.hidden; };
    if (v === 'install') { if (!open('radioPanel')) click('radioBtn'); return; }
    if (open('radioPanel')) click('radioBtn');
    if (v === 'emulator') { if (open('editor')) click('tracksBtn'); return; }
    if (!open('editor')) click('tracksBtn');                  // the editor first: a connect then lists the Pi's slots
    const panel = v === 'mine' ? 'edMine' : v === 'open' ? 'edGallery' : '';     // anything else: #tracks
    if (panel === 'edMine' && !open('edMine')) click('edMineBtn');
    else if (panel === 'edGallery' && !open('edGallery')) click('edGalBtn');
    else if (!panel && open('edMine')) click('edMineBtn');
    else if (!panel && open('edGallery')) click('edGalBtn');
  }
  // after restoreRadio's job and any other (a click while a job runs is dropped)
  function connectWhenIdle(t0) {
    t0 = t0 || Date.now();
    const r = radio();
    if (r && !r.busy) {
      if (r.ok && r.state !== 'on' && $('edConnect')) {
        $('edConnect').click();                                // connect(): no dialog, no gesture needed
        afterConnect(Date.now());
      }
      return;
    }
    if (Date.now() - t0 < 20000) setTimeout(() => connectWhenIdle(t0), 50);
  }
  // #edConnect reports to the Install panel's line (hidden unless #install): a failure goes to the
  // editor's status line too, where the user looks
  function afterConnect(t0) {
    const r = radio();
    if (r && r.busy && Date.now() - t0 < 20000) { setTimeout(() => afterConnect(t0), 50); return; }
    const from = $('rpConnStatus'), to = $('edStatus');
    if (!r || r.state === 'on' || !from || !to || !from.textContent || to.textContent) return;
    to.textContent = from.textContent;
    to.className = from.className;
  }
  function start(t0) {
    if (!ready()) {
      if (Date.now() - t0 < 30000) setTimeout(() => start(t0), 50);
      return;
    }
    view();
    connectWhenIdle();
  }
  document.addEventListener('DOMContentLoaded', () => {
    const set = (id, t) => { const n = $(id); if (n) n.textContent = t; };
    // the Connect dialog (seen only if connecting by itself failed)
    set('connectH', 'Connect the StickTime Pi');
    const ol = document.querySelector('#connectDlg ol');
    if (ol) ol.replaceChildren(Object.assign(document.createElement('li'), { textContent: 'This page\'s "radio" is the StickTime Pi: Connect reads its track slots c1 to c8 and the game on it.' }));
    const note = document.querySelector('#connectDlg .note');
    if (note) note.textContent = 'The game on the Pi picks up a track you send once it is back in its menus.';
    set('connectGo', 'Connect');
    const mo = new MutationObserver((list) => { for (const m of list) { const el = m.target.nodeType === 1 ? m.target : m.target.parentElement; const box = el && el.closest('#' + SAID.join(',#')); if (box) fix(box); } });
    for (const id of SAID) { const el = $(id); if (el) { fix(el); mo.observe(el, { childList: true, characterData: true, subtree: true }); } }
    start(Date.now());
  });
  window.addEventListener('hashchange', () => { if (ready()) view(); });
  // back from sleep or a lost WiFi: connect again
  document.addEventListener('visibilitychange', () => {
    const r = document.visibilityState === 'visible' && radio();
    if (r && r.state === 'ask') connectWhenIdle();
  });
})();
