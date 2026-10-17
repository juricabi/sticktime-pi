/* StickTime Pi settings app (web/index.html): a page per tab, all data from the JSON API of
   docs/WEB_API.md. No framework and no build step: it must load fast from the Pi and work offline
   on its hotspot. Every string from the Pi (SSIDs, song and track names, log lines) goes in as
   text, never as HTML. */
(() => {
  'use strict';

  // ---------------------------------------------------------------- small helpers
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  function h(tag, props, ...kids) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(props || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'text') el.textContent = v;
      else if (k === 'on') for (const [ev, fn] of Object.entries(v)) el.addEventListener(ev, fn);
      else if (k in el && typeof v !== 'string') el[k] = v;
      else el.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false) el.append(kid);
    return el;
  }
  const icon = (id) => {
    const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    s.setAttribute('class', 'i');
    s.setAttribute('aria-hidden', 'true');
    const u = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    u.setAttribute('href', '#i-' + id);
    s.append(u);
    return s;
  };
  const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
  const num = (v, d = 1) => (isNum(v) ? v.toFixed(d) : '–');
  const str = (v) => (typeof v === 'string' && v ? v : '–');
  function bytes(n) {
    if (!isNum(n)) return '–';
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
    return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
  }
  function dur(ms) {
    if (!isNum(ms) || ms < 0) return '–';
    const s = Math.floor(ms / 1000), m = Math.floor(s / 60), hr = Math.floor(m / 60), d = Math.floor(hr / 24);
    if (d) return d + ' d ' + (hr % 24) + ' h';
    if (hr) return hr + ' h ' + (m % 60) + ' min';
    if (m) return m + ' min ' + (s % 60) + ' s';
    return s + ' s';
  }
  function when(t) {                                        // ms since the epoch, or an ISO date
    const ms = typeof t === 'string' ? Date.parse(t) : t;
    if (!isNum(ms) || ms <= 0) return '';
    const ago = Date.now() - ms;
    if (ago < 60000) return 'just now';
    if (ago < 3600000) return Math.round(ago / 60000) + ' min ago';
    if (ago < 86400000) return Math.round(ago / 3600000) + ' h ago';
    return new Date(ms).toLocaleDateString();
  }
  const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
  const store = {
    get(k) { try { return localStorage.getItem('stp.' + k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem('stp.' + k, v); } catch (e) { /* blocked: not kept */ } },
  };
  const utf8len = (s) => new TextEncoder().encode(s).length;
  function cmpVer(a, b) {
    const x = String(a || '').split('.').map((n) => parseInt(n, 10) || 0), y = String(b || '').split('.').map((n) => parseInt(n, 10) || 0);
    for (let i = 0; i < Math.max(x.length, y.length); i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) - (y[i] || 0);
    return 0;
  }
  function download(name, text, type = 'text/plain') {
    const url = URL.createObjectURL(new Blob([text], { type: type + ';charset=utf-8' }));
    const a = h('a', { href: url, download: name });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  // ---------------------------------------------------------------- the API
  class ApiError extends Error {
    constructor(status, name, message, fields) {
      super(message);
      this.status = status;
      this.name = name;
      this.fields = fields || null;
    }
  }
  // Mutations carry X-StickTime: 1 (the server refuses them without it: a cross-site page can't add it)
  async function api(method, path, body, opts = {}) {
    const headers = {};
    if (method !== 'GET') headers['X-StickTime'] = '1';
    if (typeof body === 'string') headers['Content-Type'] = 'text/plain; charset=utf-8';
    const ctl = new AbortController(), t = setTimeout(() => ctl.abort(), opts.timeout || 10000);
    let r;
    try {
      r = await fetch(path, { method, body: method === 'GET' ? undefined : (body === undefined ? '' : body), headers, cache: 'no-store', signal: ctl.signal });
    } catch (e) {
      if (!opts.quiet) net(false);
      throw new ApiError(0, 'Offline', 'The Pi doesn\'t answer: is it on, and this device on its network?');
    } finally {
      clearTimeout(t);
    }
    if (!opts.quiet || r.ok) net(true);
    if (!r.ok) {
      let j = null;
      try { j = await r.json(); } catch (e) { /* no JSON body */ }
      const msg = (j && typeof j.message === 'string' && j.message) || 'The Pi said ' + r.status + (r.statusText ? ' ' + r.statusText : '') + '.';
      throw new ApiError(r.status, (j && j.error) || 'HTTP' + r.status, msg, j && j.fields && typeof j.fields === 'object' ? j.fields : null);
    }
    if (opts.raw) return r;
    if (r.status === 204) return null;
    const type = r.headers.get('Content-Type') || '';
    return type.includes('json') ? r.json() : r.text();
  }
  // a PUT with upload progress (fetch has none); abort() through the returned handle
  function upload(path, file, onProgress) {
    const xhr = new XMLHttpRequest();
    const done = new Promise((res, rej) => {
      xhr.open('PUT', path);
      xhr.setRequestHeader('X-StickTime', '1');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) { res(); return; }
        let j = null;
        try { j = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
        rej(new ApiError(xhr.status, (j && j.error) || 'HTTP' + xhr.status, (j && j.message) || 'The Pi said ' + xhr.status + '.'));
      };
      xhr.onerror = () => rej(new ApiError(0, 'Offline', 'The upload broke off: the Pi doesn\'t answer.'));
      xhr.onabort = () => rej(new ApiError(0, 'AbortError', 'Stopped.'));
      xhr.send(file);
    });
    return { done, abort: () => xhr.abort() };
  }

  // ---------------------------------------------------------------- connection, toasts, dialogs
  let online = null;
  function net(ok) {
    if (ok === online) return;
    online = ok;
    $('#conn').classList.toggle('on', ok);
    $('#conn').classList.toggle('off', !ok);
    $('#connText').textContent = ok ? 'Online' : 'Offline';
    $('#offline').hidden = ok;
  }
  function toast(msg, kind = 'ok', ms) {
    const t = h('div', { class: 'toast ' + kind, text: msg });
    $('#toasts').append(t);
    setTimeout(() => t.remove(), ms || (kind === 'bad' ? 7000 : 3500));
  }
  const fail = (e) => toast(e && e.message ? e.message : String(e), 'bad');
  // a question in the page's own dialog (true: OK)
  function ask(title, body, ok = 'OK', danger = false) {
    const dlg = $('#confirmDlg');
    if (typeof dlg.showModal !== 'function') return Promise.resolve(window.confirm(title + '\n\n' + body));
    $('#confirmTitle').textContent = title;
    $('#confirmBody').replaceChildren(...String(body).split('\n\n').map((p) => h('p', { text: p })));
    const yes = $('#confirmYes');
    yes.textContent = ok;
    yes.className = 'btn ' + (danger ? 'danger solid' : 'primary');
    dlg.returnValue = '';
    dlg.showModal();
    yes.focus();
    return new Promise((res) => dlg.addEventListener('close', () => res(dlg.returnValue === 'ok'), { once: true }));
  }
  function busy(btn, on) {
    if (!btn) return;
    btn.disabled = on;
    btn.classList.toggle('busy', on);
    btn.setAttribute('aria-busy', on ? 'true' : 'false');
  }

  // ---------------------------------------------------------------- waiting for the Pi to come back
  // reboot, restart, an app update: the page polls /api/status until the Pi answers again: after it
  // went away, or started after it was asked to (its uptime shorter than the wait so far; the last
  // uptime seen can't tell, it may be a few seconds old itself), or with a smaller uptime than
  // before; assume: done after that long without seeing it go away
  let waiting = false;
  async function waitForPi(title, text, opts = {}) {
    const box = $('#waitBox'), timeout = opts.timeout || 180000, t0 = opts.since || Date.now();
    waiting = true;
    box.classList.remove('done');
    box.hidden = false;
    $('#waitTitle').textContent = title;
    $('#waitText').textContent = text;
    $('#waitClose').hidden = true;
    let gone = false, up0 = lastStatus ? lastStatus.uptime_ms : null, back = null;
    await sleep(opts.first || 1200);
    while (Date.now() - t0 < timeout) {
      $('#waitTime').textContent = Math.round((Date.now() - t0) / 1000) + ' s';
      try {
        const s = await api('GET', '/api/status', undefined, { timeout: 3000, quiet: true });
        const up = s && isNum(s.uptime_ms) ? s.uptime_ms : null;
        if (gone || (up !== null && (up < Date.now() - t0 || (isNum(up0) && up < up0))) || (opts.assume && Date.now() - t0 > opts.assume)) { back = s; break; }
      } catch (e) { gone = true; }
      await sleep(1500);
    }
    waiting = false;
    if (back) {
      box.hidden = true;
      lastStatus = back;
      onStatus(back);
      return back;
    }
    box.classList.add('done');
    $('#waitTitle').textContent = 'The Pi isn\'t back yet';
    $('#waitText').textContent = 'Look at the goggles or the screen. The Pi may be on another network now: find it at ' + hostUrl() +
      ' or at the address on the game\'s main menu. If it made its own hotspot, join "' + (S.values ? S.values.hotspot_ssid || 'StickTime' : 'StickTime') + '".';
    $('#waitClose').hidden = false;
    $('#waitClose').focus();
    return null;
  }
  const hostUrl = () => 'http://' + ((lastStatus && lastStatus.network && lastStatus.network.hostname) || (S.values && S.values.hostname) || 'sticktime') + '.local';
  async function power(what) {
    const say = {
      reboot: ['Reboot the Pi?', 'It is back in about 30 seconds.', 'Reboot'],
      shutdown: ['Shut down the Pi?', 'To start it again, unplug it and plug it back in.', 'Shut down'],
      restart: ['Restart the game?', 'The game and this page are back in a few seconds.', 'Restart'],
    }[what];
    if (!await ask(say[0], say[1], say[2], what === 'shutdown')) return;
    const sent = Date.now();
    try { await api('POST', '/api/' + what); } catch (e) { fail(e); return; }
    $('#rebootBanner').hidden = true;
    if (what === 'shutdown') {
      const box = $('#waitBox');
      box.hidden = false;
      box.classList.add('done');
      $('#waitTitle').textContent = 'The Pi is shutting down';
      $('#waitText').textContent = 'Unplug it when its green light has stopped blinking (about 10 seconds).';
      $('#waitTime').textContent = '';
      $('#waitClose').hidden = false;
      waiting = true;
      return;
    }
    const back = await waitForPi(what === 'reboot' ? 'Rebooting…' : 'Restarting the game…',
      what === 'reboot' ? 'The Pi is starting again. This page waits for it.' : 'The game starts again in a moment.',
      { timeout: what === 'reboot' ? 240000 : 60000, first: what === 'reboot' ? 4000 : 1000, since: sent });
    if (back) { toast(what === 'reboot' ? 'The Pi is back.' : 'The game is back.'); loadSettings(); }
  }
  $('#waitClose').addEventListener('click', () => { $('#waitBox').hidden = true; waiting = false; });
  $('#rebootNow').addEventListener('click', () => power('reboot'));
  $('#rebootLater').addEventListener('click', () => { $('#rebootBanner').hidden = true; });
  $('#restartBtn').addEventListener('click', () => power('restart'));
  $('#rebootBtn').addEventListener('click', () => power('reboot'));
  $('#shutdownBtn').addEventListener('click', () => power('shutdown'));

  // ---------------------------------------------------------------- settings
  // GET /api/settings: values, the keys' metadata (kind, choices, live, reboot, help), warnings
  const S = { values: null, keys: [], byKey: new Map(), warnings: '', error: '' };
  const SECRET = (k) => k && k.kind === 'secret';
  // the host restarts for these (the server's "restart" flag; this list only for a server without it)
  const RESTART = new Set(['aspect', 'av_mode', 'hdmi_mode', 'margin', 'fonts', 'audio_out']);
  const restarts = (k) => (typeof k.restart === 'boolean' ? k.restart : RESTART.has(k.key));
  // how a saved key takes effect: at once, after a reboot, by a restart of the game, or when a helper
  // next runs (the updater's keys): no flag then
  function applies(k) {
    if (k.live) return ['live', 'at once', 'Applies at once'];
    if (k.reboot) return ['reboot', 'after reboot', 'Applies when the Pi has restarted (Reboot now offers it)'];
    if (restarts(k)) return ['restart', 'restarts the game', 'The game restarts for it (a second or two)'];
    return null;
  }
  const PLACED = new Set(['wifi_ssid', 'wifi_password', 'script',                   // set elsewhere: Join, the script list,
    'vtx_band', 'vtx_channel', 'vtx_freq', 'vtx_power', 'vtx_pit']);                // the VTX card's pickers
  const LABEL = {
    video: 'Output', tv: 'TV standard', aspect: 'Picture shape', av_mode: 'Composite lines', hdmi_mode: 'HDMI mode', margin: 'Border',
    fonts: 'Text size', show_ip: 'Show the web address', wifi_country: 'Country', hotspot: 'Hotspot', hotspot_ssid: 'Hotspot name',
    hotspot_password: 'Hotspot password', input: 'Stick source', channel_order: 'Channel order', invert: 'Reverse', pause_channel: 'Pause switch',
    stick_menus: 'Menus by sticks', wifi_module_ip: 'Module address', wifi_interval_us: 'Packet interval', bluetooth: 'Bluetooth',
    latency: 'Camera prediction', stick_mode: 'Stick mode', script: 'Script', stats: 'Timing stats', sd_write: 'The web page may change',
    sound: 'Sound', volume: 'Volume', music: 'Music', music_volume: 'Music volume', music_source: 'Music from', audio_out: 'Output', audio_mono: 'Mono',
    hostname: 'Name', ssh: 'SSH', password: 'Password of user pi', auto_update: 'Automatic updates', github_token: 'GitHub token',
    vtx: 'VTX control', vtx_port: 'Pin', vtx_send_only: 'Send only',
  };
  const CHOICE = {
    video: { av: 'AV (composite)', hdmi: 'HDMI' },
    tv: { ntsc: 'NTSC', pal: 'PAL', pal60: 'PAL-60', 'ntsc-j': 'NTSC-J (Japan)', 'pal-m': 'PAL-M (Brazil)', 'pal-n': 'PAL-N', 'ntsc-443': 'NTSC 4.43', secam: 'SECAM' },
    av_mode: { interlaced: 'Interlaced', progressive: 'Progressive' },
    fonts: { auto: 'Auto', std: 'Standard radio (480×272)', sml: 'Small radio', lrg: 'Large radio (800×480)', pc: 'PC (1280×720)', hd: 'HD (1920×1080)' },
    input: { auto: 'Auto: the last one that moved', wifi: 'ELRS / Crossfire WiFi', usb: 'USB radio', bluetooth: 'Bluetooth', gamepad: 'Gamepad', joystick: 'Joystick', keyboard: 'Keyboard' },
    sd_write: { game: 'Tracks + game', tracks: 'Tracks only', all: 'Everything', off: 'Nothing' },
    music_source: { generated: 'Generated', songs: 'My songs' },
    audio_out: { auto: 'Auto', jack: 'Jack', hdmi: 'HDMI', off: 'Off' },
    show_ip: { auto: 'Auto', on: 'On', off: 'Off' },
    hotspot: { auto: 'Auto', on: 'On', off: 'Off' },
    vtx: { off: 'Off', auto: 'Auto', smartaudio: 'SmartAudio', tramp: 'Tramp' },
    vtx_port: { gpio4: 'One pin: GPIO4, pin 7', gpio17: 'One pin: GPIO17, pin 11', gpio22: 'One pin: GPIO22, pin 15',
      gpio23: 'One pin: GPIO23, pin 16', gpio27: 'One pin: GPIO27, pin 13', uart3: 'Two pins: UART3, TX pin 7 + RX pin 29',
      uart4: 'Two pins: UART4, TX pin 24 + RX pin 21', uart5: 'Two pins: UART5, TX pin 32 + RX pin 33' },
  };
  const HELP_MORE = {
    av_mode: 'Progressive has half the lines and no flicker.',
    sd_write: 'Tracks + game: the track editor and its Install. Everything: any file in the game\'s SD folder.',
    channel_order: 'AETR for ELRS and most radios; TAER for some Spektrum and Futaba setups.',
    pause_channel: 'Flip this switch in flight to pause.',
    latency: 'Seconds the camera looks ahead to hide the delay of the goggles, like 0.02.',
    music: 'Plays while you fly: generated live, or your own songs (below).',
    password: 'Set it before you turn SSH on. It is applied at the next boot and then removed from the file.',
    hotspot_password: 'Not set: the hotspot\'s password is sticktime.',
    vtx: 'One pin works at once; the two-pin UART mode needs a reboot when you turn it on (the Pi turns the UART on as it starts).',
    vtx_port: 'One pin (one wire through 1 kΩ) is the usual way; two pins (a UART) are the most reliable if one pin gives errors.',
    vtx_send_only: 'For a VTX whose answers can\'t be read: this page then can\'t show what the VTX is set to.',
  };
  // fields that only matter for one output
  const WHEN = { tv: (v) => v.video === 'av', av_mode: (v) => v.video === 'av', hdmi_mode: (v) => v.video === 'hdmi' };
  const SLIDERS = new Set(['volume', 'music_volume', 'margin']);
  // the server's one-line help (no full stop), then ours
  function helpText(k) {
    let t = typeof k.help === 'string' ? k.help.trim() : '';
    if (t.toLowerCase() === label(k.key).toLowerCase()) t = '';
    if (t && HELP_MORE[k.key] && !/[.!?]$/.test(t)) t += '.';
    return [t, HELP_MORE[k.key] || ''].filter(Boolean).join(' ');
  }
  const isOn = (v) => /^(on|yes|true|1)$/i.test(String(v || '').trim());
  const label = (key) => LABEL[key] || key.replace(/_/g, ' ').replace(/^./, (c) => c.toUpperCase());
  function range(k) {
    let lo = isNum(k.min) ? k.min : null, hi = isNum(k.max) ? k.max : null;
    if ((lo === null || hi === null) && Array.isArray(k.choices) && k.choices.length === 2 && k.kind !== 'enum') {
      lo = parseFloat(k.choices[0]);
      hi = parseFloat(k.choices[1]);
    }
    return Number.isFinite(lo) && Number.isFinite(hi) ? [lo, hi] : null;
  }
  // a value as a settings line ("key = value"): quoted when it needs it (settings.h)
  function line(key, v) {
    v = String(v);
    if (/^\s|\s$|#|^"/.test(v)) v = '"' + v.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
    return key + ' = ' + v;
  }
  // what the server would refuse anyway, said before sending
  function check(k, v) {
    if (/[\x00-\x1f\x7f]/.test(v)) return 'no line breaks or control characters';
    if (k.kind === 'enum' && Array.isArray(k.choices) && k.choices.length && !k.choices.includes(v)) return 'must be one of ' + k.choices.join(', ');
    if ((k.kind === 'int' || k.kind === 'num') && v !== '') {
      const n = Number(v), r = range(k);
      if (!/^-?\d+(\.\d+)?$/.test(v.trim()) || !Number.isFinite(n) || (k.kind === 'int' && !Number.isInteger(n))) return k.kind === 'int' ? 'a whole number' : 'a number';
      if (r && (n < r[0] || n > r[1])) return r[0] + ' to ' + r[1];
    }
    if (k.key === 'hotspot_password' && v && (utf8len(v) < 8 || utf8len(v) > 63)) return '8 to 63 characters';
    return '';
  }
  function parseSettings(text) {                             // the same rules as settings.c
    const out = new Map();
    for (const raw of text.replace(/^﻿/, '').split(/\r?\n/)) {
      const t = raw.trim();
      if (!t || t[0] === '#' || !t.includes('=')) continue;
      const i = t.indexOf('=');
      const k = t.slice(0, i).trim().toLowerCase();
      let v = t.slice(i + 1).trim();
      if (v[0] === '"') {
        let o = '', j = 1;
        for (; j < v.length && v[j] !== '"'; j++) { if (v[j] === '\\' && j + 1 < v.length) j++; o += v[j]; }
        v = o;
      } else {
        const m = /[ \t]#/.exec(v);
        if (m) v = v.slice(0, m.index).trimEnd();
      }
      if (k) out.set(k, v);
    }
    return out;
  }

  // one control per key, by its kind; get() is the value as text ('' = empty)
  function makeField(k, form) {
    const key = k.key, id = 'f-' + form.name + '-' + key, val = S.values[key] === undefined || S.values[key] === null ? '' : String(S.values[key]);
    const help = h('p', { class: 'help', id: id + '-help', text: helpText(k) });
    const err = h('p', { class: 'err', id: id + '-err', 'aria-live': 'polite' });
    const how = applies(k);
    const flag = how ? h('span', { class: 'flag ' + how[0], text: how[1], title: how[2] }) : null;
    const changed = h('span', { class: 'flag changed', text: 'changed', hidden: true });
    const desc = id + '-help ' + id + '-err';
    const f = { key, k, orig: val, root: null, get: null, set: null, input: null, changed };
    const top = (ctl, asGroup) => h('div', { class: 'field-top' },
      h('span', { class: 'field-title' }, asGroup ? h('span', { class: 'lbl', id: id + '-lbl', text: label(key) }) : h('label', { for: id, text: label(key) }), flag, changed), ctl || null);
    // radios and check boxes: a labelled group (not fieldset/legend: WebKit draws a floated legend into the box's border)
    const group = (role, ...kids) => h('div', { class: 'field', role, 'aria-labelledby': id + '-lbl', 'aria-describedby': desc }, top(null, true), ...kids);
    const range2 = range(k);
    const changedNow = () => form.changed(f);

    if (k.kind === 'bool') {
      const cb = h('input', { type: 'checkbox', role: 'switch', id, 'aria-describedby': desc, checked: isOn(val), on: { change: changedNow } });
      f.orig = isOn(val) ? 'on' : 'off';
      f.get = () => (cb.checked ? 'on' : 'off');
      f.set = (v) => { cb.checked = isOn(v); };
      f.input = cb;
      f.root = h('div', { class: 'field' }, top(cb), help, err);
    } else if (k.kind === 'secret') {
      const isSet = !!S.values[key + '_set'];
      const state = h('span', { class: 'state' + (isSet ? ' set' : ''), text: isSet ? 'Set' : 'Not set' });
      const inp = h('input', { type: 'password', id, autocomplete: 'new-password', 'aria-describedby': desc, spellcheck: 'false', autocapitalize: 'off', on: { input: changedNow } });
      const reveal = h('button', { class: 'btn sm ghost', type: 'button', text: 'Show', 'aria-pressed': 'false', 'aria-controls': id });
      reveal.addEventListener('click', () => { const show = inp.type === 'password'; inp.type = show ? 'text' : 'password'; reveal.textContent = show ? 'Hide' : 'Show'; reveal.setAttribute('aria-pressed', String(show)); });
      const edit = h('div', { class: 'pass', hidden: true }, inp, reveal);
      const change = h('button', { class: 'btn sm', type: 'button', text: isSet ? 'Change' : 'Set' });
      const clear = h('button', { class: 'btn sm ghost', type: 'button', text: 'Clear', hidden: !isSet });
      const cancel = h('button', { class: 'btn sm ghost', type: 'button', text: 'Cancel', hidden: true });
      let clearing = false;
      const mode = (editing) => {
        edit.hidden = !editing; change.hidden = editing; cancel.hidden = !editing && !clearing; clear.hidden = editing || clearing || !isSet;
        if (editing) inp.focus();
      };
      change.addEventListener('click', () => mode(true));
      cancel.addEventListener('click', () => { inp.value = ''; clearing = false; state.textContent = isSet ? 'Set' : 'Not set'; mode(false); changedNow(); });
      clear.addEventListener('click', () => { clearing = true; state.textContent = 'Will be cleared'; mode(false); changedNow(); });
      f.orig = '';
      f.get = () => (clearing ? '\u0000clear' : edit.hidden ? '' : inp.value);
      f.set = () => { inp.value = ''; clearing = false; mode(false); };
      f.input = inp;
      f.secret = true;
      f.root = h('div', { class: 'field' }, top(null), h('div', { class: 'secret' }, state, change, clear, cancel), edit, help, err);
    } else if (k.kind === 'enum' && Array.isArray(k.choices) && k.choices.length) {
      const names = CHOICE[key] || {};
      const lab = (c) => names[c] || c;
      const seg = k.choices.length <= 4 && k.choices.every((c) => lab(c).length <= 14);
      if (seg) {
        const radios = k.choices.map((c) => h('input', { type: 'radio', name: id, value: c, checked: c === val, on: { change: changedNow } }));
        f.get = () => { const r = radios.find((x) => x.checked); return r ? r.value : val; };
        f.set = (v) => radios.forEach((r) => { r.checked = r.value === v; });
        f.input = radios.find((r) => r.checked) || radios[0];
        f.root = group('radiogroup', h('div', { class: 'seg' }, k.choices.map((c, i) => h('label', {}, radios[i], h('span', { text: lab(c) })))), help, err);
      } else {
        const sel = h('select', { id, 'aria-describedby': desc, on: { change: changedNow } },
          k.choices.map((c) => h('option', { value: c, text: lab(c), selected: c === val })));
        if (!k.choices.includes(val)) sel.prepend(h('option', { value: val, text: val ? val + ' (not valid)' : '–', selected: true, disabled: true }));
        f.get = () => sel.value;
        f.set = (v) => { sel.value = v; };
        f.input = sel;
        f.root = h('div', { class: 'field' }, top(null), sel, help, err);
      }
    } else if (key === 'invert') {
      const AX = [['ail', 'Aileron'], ['ele', 'Elevator'], ['thr', 'Throttle'], ['rud', 'Rudder']];
      const have = new Set(val.split(/[\s,]+/).map((s) => s.toLowerCase()).filter(Boolean));
      const boxes = AX.map(([a]) => h('input', { type: 'checkbox', value: a, checked: have.has(a), on: { change: changedNow } }));
      f.orig = AX.map(([a]) => a).filter((a) => have.has(a)).join(', ');
      f.get = () => boxes.filter((b) => b.checked).map((b) => b.value).join(', ');
      f.set = (v) => { const s = new Set(String(v).split(/[\s,]+/)); boxes.forEach((b) => { b.checked = s.has(b.value); }); };
      f.input = boxes[0];
      f.root = group('group', h('div', { class: 'checks' }, AX.map(([a, n], i) => h('label', {}, boxes[i], h('span', { text: n })))), help, err);
    } else if (key === 'pause_channel' && range2) {
      const opts = [];
      for (let c = range2[0]; c <= range2[1]; c++) opts.push(h('option', { value: String(c), text: c === 0 ? 'Off' : 'CH' + c, selected: String(c) === val }));
      const sel = h('select', { id, 'aria-describedby': desc, on: { change: changedNow } }, opts);
      f.get = () => sel.value;
      f.set = (v) => { sel.value = v; };
      f.input = sel;
      f.root = h('div', { class: 'field' }, top(null), sel, help, err);
    } else if ((k.kind === 'int') && range2 && range2[1] - range2[0] <= 3 && range2[1] - range2[0] >= 1) {
      const vals = [];
      for (let c = range2[0]; c <= range2[1]; c++) vals.push(String(c));
      const radios = vals.map((c) => h('input', { type: 'radio', name: id, value: c, checked: c === val, on: { change: changedNow } }));
      f.get = () => { const r = radios.find((x) => x.checked); return r ? r.value : val; };
      f.set = (v) => radios.forEach((r) => { r.checked = r.value === v; });
      f.input = radios.find((r) => r.checked) || radios[0];
      f.root = group('radiogroup', h('div', { class: 'seg' }, vals.map((c, i) => h('label', {}, radios[i], h('span', { text: (key === 'stick_mode' ? 'Mode ' : '') + c })))), help, err);
    } else if ((k.kind === 'int' || k.kind === 'num') && range2 && SLIDERS.has(key)) {
      const out = h('output', { for: id });
      const unit = '%';
      const r = h('input', { type: 'range', id, min: range2[0], max: range2[1], step: k.kind === 'int' ? 1 : 'any', value: val || range2[0], 'aria-describedby': desc });
      const show = () => { out.textContent = r.value + unit; r.setAttribute('aria-valuetext', r.value + ' percent'); };
      r.addEventListener('input', () => { show(); changedNow(); });
      show();
      f.get = () => r.value;
      f.set = (v) => { r.value = v; show(); };
      f.input = r;
      f.root = h('div', { class: 'field' }, top(null), h('div', { class: 'range' }, r, out), help, err);
    } else {
      // text and numbers (a datalist where common values exist)
      const lists = { channel_order: ['AETR', 'TAER', 'ETAR', 'RETA', 'AERT'], hdmi_mode: ['auto', '1280x720', '1280x720@60', '1920x1080', '1920x1080@60', '720x480', '720x576'] };
      const attrs = { id, 'aria-describedby': desc, spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', value: val, on: { input: changedNow } };
      let hint = null;
      if (k.kind === 'int' || k.kind === 'num') {
        Object.assign(attrs, { type: 'text', inputmode: k.kind === 'int' ? 'numeric' : 'decimal' });
        if (range2) attrs.placeholder = range2[0] + ' to ' + range2[1];
      } else attrs.type = 'text';
      if (key === 'latency') attrs.inputmode = 'decimal';
      if (key === 'wifi_module_ip') { attrs.inputmode = 'decimal'; attrs.placeholder = 'found by its broadcast'; }
      if (key === 'wifi_country') { attrs.list = 'countries'; attrs.maxlength = '2'; attrs.autocapitalize = 'characters'; }
      if (key === 'hostname') attrs.maxlength = '63';
      let dl = null;
      if (lists[key]) { dl = h('datalist', { id: id + '-list' }, lists[key].map((v) => h('option', { value: v }))); attrs.list = id + '-list'; }
      const inp = h('input', attrs);
      if (key === 'wifi_interval_us') {
        hint = h('span', { class: 'note' });
        const hz = () => { const n = parseInt(inp.value, 10); hint.textContent = n > 0 ? '= ' + (Math.round(1e7 / n) / 10) + ' Hz' : ''; };
        inp.addEventListener('input', hz);
        hz();
      }
      if (key === 'channel_order' || key === 'wifi_country') inp.addEventListener('blur', () => { const u = inp.value.toUpperCase(); if (u !== inp.value) { inp.value = u; changedNow(); } });
      f.get = () => inp.value.trim();
      f.set = (v) => { inp.value = v; };
      f.input = inp;
      f.root = h('div', { class: 'field' }, top(null), hint ? h('div', { class: 'range' }, inp, hint) : inp, dl, help, err);
    }
    f.err = err;
    return f;
  }

  const forms = [];
  class SettingsForm {
    constructor(el) {
      this.el = el;
      this.name = el.dataset.form;
      this.spec = el.dataset.keys;
      this.autosave = el.hasAttribute('data-autosave');
      this.fields = new Map();
      this.saving = false;
      this.again = false;
      this.timer = 0;
      el.addEventListener('submit', (e) => { e.preventDefault(); this.save(); });
      forms.push(this);
    }
    keys() {
      if (this.spec !== '*') return this.spec.split(',').map((k) => S.byKey.get(k)).filter(Boolean);
      const elsewhere = new Set(forms.filter((f) => f !== this && f.spec !== '*').flatMap((f) => f.spec.split(',')));
      return S.keys.filter((k) => !elsewhere.has(k.key) && !PLACED.has(k.key));
    }
    // (re)built from S.values, unless the user is editing it: their edits stay
    render(force) {
      if (!force && this.dirty().length) return;
      if (S.error && !S.values) {
        this.el.replaceChildren(h('div', { class: 'alert bad' }, icon('warn'), h('div', {}, 'Couldn\'t read the settings: ' + S.error + ' ',
          h('button', { class: 'btn sm', type: 'button', text: 'Try again', on: { click: () => loadSettings() } }))));
        return;
      }
      if (!S.values) return;
      const keys = this.keys();
      if (this.spec === '*') $('#moreCard').hidden = !keys.length;
      // keyboard focus survives the rebuild (the same control by id)
      const act = document.activeElement, focusId = act && this.el.contains(act) && act.id ? act.id : '';
      this.fields.clear();
      const kids = keys.map((k) => { const f = makeField(k, this); this.fields.set(k.key, f); return f.root; });
      this.formErr = h('div', { class: 'alert bad form-err', role: 'alert', hidden: true });
      this.count = h('span');
      this.bar = h('div', { class: 'savebar', hidden: true }, this.count,
        h('button', { class: 'btn ghost sm', type: 'button', text: 'Undo', on: { click: () => this.render(true) } }),
        h('button', { class: 'btn primary sm', type: 'submit', text: 'Save' }));
      this.el.replaceChildren(...kids, this.formErr, this.autosave ? '' : this.bar);
      this.conditions();
      const again = focusId && document.getElementById(focusId);
      if (again && again.offsetParent) again.focus({ preventScroll: true });
    }
    values() {
      const v = {};
      for (const [k, f] of this.fields) v[k] = f.get();
      return Object.assign({}, S.values, v);
    }
    conditions() {
      const v = this.values();
      for (const [k, f] of this.fields) if (WHEN[k]) f.root.hidden = !WHEN[k](v);
    }
    dirty() {
      const out = [];
      for (const [k, f] of this.fields) {
        const v = f.get();
        if (f.secret ? v !== '' : v !== f.orig) out.push([k, v]);
      }
      return out;
    }
    changed(f) {
      f.changed.hidden = f.secret ? f.get() === '' : f.get() === f.orig;
      f.root.classList.remove('invalid');
      f.err.textContent = '';
      this.conditions();
      const n = this.dirty().length;
      if (this.autosave) {
        clearTimeout(this.timer);
        if (n) this.timer = setTimeout(() => this.save(), f.input && f.input.type === 'range' ? 350 : 0);
        return;
      }
      this.bar.hidden = !n;
      this.count.textContent = n === 1 ? '1 change' : n + ' changes';
    }
    showErrors(fields) {
      let first = null;
      const other = [];
      for (const [k, msg] of Object.entries(fields)) {
        const f = this.fields.get(k);
        if (!f) { other.push(k + ': ' + msg); continue; }
        f.root.classList.add('invalid');
        f.err.textContent = msg;
        if (f.input) f.input.setAttribute('aria-invalid', 'true');
        if (!first) first = f;
      }
      this.formErr.hidden = !other.length;
      this.formErr.textContent = other.join('\n');
      if (first && first.input) first.input.focus();
    }
    async save() {
      if (this.saving) { this.again = true; return; }
      const todo = this.dirty();
      if (!todo.length) return;
      const errs = {};
      for (const [k, v] of todo) {
        const e = v === '\u0000clear' ? '' : check(this.fields.get(k).k, v);
        if (e) errs[k] = e;
      }
      for (const f of this.fields.values()) { f.root.classList.remove('invalid'); f.err.textContent = ''; if (f.input) f.input.removeAttribute('aria-invalid'); }
      this.formErr.hidden = true;
      if (Object.keys(errs).length) { this.showErrors(errs); return; }
      const body = todo.map(([k, v]) => (v === '\u0000clear' ? k + ' =\n' + k + '_clear = 1' : line(k, v))).join('\n') + '\n';
      this.saving = true;
      const btn = this.bar && $('button[type=submit]', this.bar);
      busy(btn, true);
      let r, sent;
      try {
        sent = Date.now();
        r = await api('POST', '/api/settings', body);
      } catch (e) {
        this.saving = false;
        busy(btn, false);
        if (e.fields) this.showErrors(e.fields);
        else { this.formErr.hidden = false; this.formErr.textContent = 'Not saved: ' + e.message; }
        if (this.autosave) toast('Not saved: ' + e.message, 'bad');
        return;
      }
      this.saving = false;
      busy(btn, false);
      for (const [k, v] of todo) {
        const f = this.fields.get(k);
        if (f.secret) S.values[k + '_set'] = v !== '\u0000clear' && v !== '';
        else S.values[k] = v;
      }
      saved(r || {}, todo.map(([k]) => k), sent);
      this.again = false;
      if (this.autosave) {
        // never rebuilt under the user's finger (a slider being dragged): the saved values are the new
        // starting point, and what changed meanwhile is saved next
        for (const [k, v] of todo) this.fields.get(k).orig = v;
        for (const f of this.fields.values()) f.changed.hidden = f.get() === f.orig;
        if (this.dirty().length) this.save();
      } else this.render(true);
      rerender(this);
      if (!this.autosave && !(r && r.restart)) refresh();
    }
  }
  // the values as the Pi stored them (it normalizes: TAER for taer, on for yes), read back after a save;
  // forms being edited and the Sound sliders keep what they show
  async function refresh() {
    let j;
    try { j = await api('GET', '/api/settings', undefined, { quiet: true }); } catch (e) { return; }
    if (!j || !j.values || typeof j.values !== 'object') return;
    S.values = j.values;
    S.warnings = typeof j.warnings === 'string' ? j.warnings : S.warnings;
    for (const f of forms) if (!f.autosave) f.render();
    renderFile();
  }
  // after a save: what it takes to apply (the response says reboot / restart)
  function saved(r, keys, sent) {
    const boot = keys.map((k) => S.byKey.get(k)).filter((k) => k && k.reboot);
    if (r.reboot || boot.length) {
      $('#rebootText').textContent = 'Saved. ' + (boot.length ? boot.map((k) => label(k.key)).join(', ') + ': applies' : 'This applies') + ' when the Pi has restarted.';
      $('#rebootBanner').hidden = false;
    }
    toast(keys.length === 1 ? label(keys[0]) + ' saved.' : 'Saved.');
    document.dispatchEvent(new CustomEvent('settings', { detail: keys }));
    if (r.restart) waitForPi('Restarting the game…', 'It applies the new settings: back in a moment.', { timeout: 45000, assume: 8000, since: sent });
  }
  function rerender(except) {
    for (const f of forms) if (f !== except) f.render();
    renderFile();
  }
  async function loadSettings() {
    try {
      const j = await api('GET', '/api/settings');
      S.values = j && typeof j.values === 'object' && j.values ? j.values : {};
      S.keys = Array.isArray(j.keys) ? j.keys.filter((k) => k && typeof k.key === 'string') : [];
      S.byKey = new Map(S.keys.map((k) => [k.key, k]));
      S.warnings = typeof j.warnings === 'string' ? j.warnings : '';
      S.error = '';
    } catch (e) {
      S.error = e.message;
    }
    rerender(null);
    document.dispatchEvent(new CustomEvent('settings', { detail: null }));
  }
  $$('form.settings').forEach((el) => new SettingsForm(el));

  // the settings as sticktime.txt text (System): view, download, edit and apply
  let fileShown = '';
  function settingsText() {
    if (!S.values) return '';
    const out = ['# StickTime Pi settings as the web page sees them (the passwords are left out)', ''];
    for (const k of S.keys) {
      if (k.help) out.push('# ' + k.help);
      if (SECRET(k)) out.push('# ' + k.key + ' is ' + (S.values[k.key + '_set'] ? 'set' : 'not set') + ': to change it, add a line ' + k.key + ' = ...');
      else out.push(line(k.key, S.values[k.key] === undefined ? '' : S.values[k.key]));
    }
    const extra = Object.keys(S.values).filter((key) => !S.byKey.has(key) && !(key.endsWith('_set') && SECRET(S.byKey.get(key.slice(0, -4)))));
    if (extra.length) { out.push('', '# other keys in the file'); for (const key of extra) out.push(line(key, S.values[key])); }
    return out.join('\n') + '\n';
  }
  function renderFile(force) {
    const ta = $('#fileText');
    if (!force && ta.value !== fileShown) return;               // being edited
    fileShown = settingsText();
    ta.value = fileShown;
    $('#fileWarn').hidden = !S.warnings;
    $('#fileWarn').textContent = S.warnings ? 'The file on the SD card has lines the Pi skips:\n' + S.warnings : '';
  }
  $('#fileSave').addEventListener('click', () => download('sticktime.txt', $('#fileText').value));
  $('#fileReset').addEventListener('click', () => { renderFile(true); $('#fileErr').hidden = true; });
  $('#fileApply').addEventListener('click', async () => {
    const now = parseSettings($('#fileText').value), todo = [];
    for (const [key, v] of now) {
      const k = S.byKey.get(key);
      if (key.endsWith('_set') && SECRET(S.byKey.get(key.slice(0, -4)))) continue;
      if (/[\x00-\x1f\x7f]/.test(v)) continue;
      if (SECRET(k) ? v !== '' : String(S.values[key] === undefined ? '' : S.values[key]) !== v) todo.push([key, v]);
    }
    const box = $('#fileErr');
    box.hidden = true;
    if (!todo.length) { toast('Nothing changed.'); return; }
    const btn = $('#fileApply');
    busy(btn, true);
    try {
      const sent = Date.now();
      const r = await api('POST', '/api/settings', todo.map(([k, v]) => line(k, v)).join('\n') + '\n');
      busy(btn, false);
      fileShown = '';
      if (!(r && r.restart)) await loadSettings();
      renderFile(true);
      saved(r || {}, todo.map(([k]) => k), sent);
    } catch (e) {
      busy(btn, false);
      box.hidden = false;
      box.textContent = e.fields ? Object.entries(e.fields).map(([k, m]) => k + ': ' + m).join('\n') : 'Not saved: ' + e.message;
    }
  });

  // ---------------------------------------------------------------- status (every page uses some of it)
  let lastStatus = null;
  const STATE = { unknown: 'Starting', menu: 'In the menu', setup: 'Settings', count: 'Countdown', fly: 'Flying', crashed: 'Crashed', ready: 'Ready', paused: 'Paused', done: 'Finished' };
  const KIND = { wifi: 'WiFi', usb: 'USB', bluetooth: 'Bluetooth', gamepad: 'Gamepad', joystick: 'Joystick', keyboard: 'Keyboard', web: 'Web' };
  function throttled(hex) {
    const n = parseInt(hex, 16);
    if (!Number.isFinite(n)) return '–';
    if (!n) return 'OK';
    const now = [], past = [];
    if (n & 1) now.push('too low voltage'); if (n & 2) now.push('CPU slowed'); if (n & 4) now.push('throttled'); if (n & 8) now.push('hot');
    if (n & 0x10000) past.push('low voltage'); if (n & 0x20000) past.push('slowed'); if (n & 0x40000) past.push('throttled'); if (n & 0x80000) past.push('hot');
    return now.length ? 'Now: ' + now.join(', ') + (now.includes('too low voltage') ? ' (use a 3 A supply)' : '') : 'OK now (earlier: ' + past.join(', ') + ')';
  }
  const g = (o, ...p) => p.reduce((a, k) => (a && typeof a === 'object' ? a[k] : undefined), o);
  const SF = {
    fps: (s) => num(s.fps, 1),
    frameLine: (s) => (isNum(g(s, 'frame_ms', 'avg')) ? num(g(s, 'frame_ms', 'avg'), 1) + ' ms per frame' : ''),
    latency: (s) => num(s.latency_ms, 1),
    state: (s) => STATE[g(s, 'game', 'state_name')] || str(g(s, 'game', 'state_name')),
    gameLine: (s) => (g(s, 'game', 'loaded') === false ? 'not loaded' : g(s, 'game', 'error') ? 'stopped: error' : 'StickTime ' + str(s.game_version)),
    inputKind: (s) => KIND[g(s, 'input', 'kind')] || str(g(s, 'input', 'kind')),
    inputRate: (s) => { const src = (g(s, 'input', 'sources') || [])[g(s, 'input', 'active')]; return src && isNum(src.rate_hz) && src.rate_hz > 0 ? Math.round(src.rate_hz) + ' Hz' : ''; },
    inputName: (s) => { const src = (g(s, 'input', 'sources') || [])[g(s, 'input', 'active')]; return src ? (src.name || '') + (g(s, 'input', 'live') ? '' : ' · not moving') : 'no source'; },
    videoDesc: (s) => str(g(s, 'video', 'desc')),
    frameMs: (s) => num(g(s, 'frame_ms', 'avg'), 2) + ' ms avg · ' + num(g(s, 'frame_ms', 'max'), 1) + ' max',
    runMs: (s) => num(g(s, 'run_ms', 'avg'), 2) + ' ms avg · ' + num(g(s, 'run_ms', 'p95'), 1) + ' p95',
    presentMs: (s) => num(s.present_ms, 2) + ' ms',
    frames: (s) => (isNum(s.frames) ? s.frames.toLocaleString() : '–'),
    missed: (s) => (isNum(s.missed) ? s.missed.toLocaleString() : '–'),
    luaKb: (s) => (isNum(g(s, 'game', 'lua_kb')) ? g(s, 'game', 'lua_kb') + ' KB' : '–'),
    wjModule: (s) => g(s, 'wifi_joystick', 'module') || 'none found yet',
    wjState: (s) => str(g(s, 'wifi_joystick', 'state')),
    // pauses of 0.25 s or more in the stick data since the start (the log says when)
    wjGaps: (s) => (isNum(g(s, 'wifi_joystick', 'gaps')) ? (g(s, 'wifi_joystick', 'gaps') ? g(s, 'wifi_joystick', 'gaps') + ', longest ' + (g(s, 'wifi_joystick', 'gap_max_ms') / 1000).toFixed(2) + ' s' : 'none') : '–'),
    wjRate: (s) => (isNum(g(s, 'wifi_joystick', 'rate_hz')) ? Math.round(g(s, 'wifi_joystick', 'rate_hz')) + ' Hz' + (g(s, 'wifi_joystick', 'dropped') ? ', ' + g(s, 'wifi_joystick', 'dropped') + ' dropped' : '') : '–'),
    netAddr: (s) => str(g(s, 'network', 'ip')) + (g(s, 'network', 'iface') ? ' (' + g(s, 'network', 'iface') + ')' : ''),
    netWifi: (s) => (g(s, 'network', 'mode') === 'hotspot' ? 'own hotspot' : str(g(s, 'network', 'ssid'))),
    netName: (s) => (g(s, 'network', 'hostname') ? g(s, 'network', 'hostname') + '.local' : '–'),
    netMode: (s) => ({ client: 'Joined a network', hotspot: 'Own hotspot', ap: 'Own hotspot', module: 'The module\'s WiFi' })[g(s, 'network', 'mode')] || str(g(s, 'network', 'mode')),
    netSsid: (s) => str(g(s, 'network', 'ssid')),
    cpuTemp: (s) => (isNum(g(s, 'system', 'cpu_temp')) ? num(g(s, 'system', 'cpu_temp'), 1) + ' °C' : '–'),
    throttled: (s) => throttled(g(s, 'system', 'throttled')),
    uptime: (s) => dur(s.uptime_ms),
    audio: (s) => (g(s, 'audio', 'device') ? g(s, 'audio', 'device') + (g(s, 'audio', 'ok') === false ? ' (not working)' : '') : '–'),
    gameVersion: (s) => str(s.game_version),
    appVersion: (s) => str(s.version),
    music: (s) => g(s, 'audio', 'music') || 'Nothing',
    audioDev: (s) => (g(s, 'audio', 'device') ? 'Output: ' + g(s, 'audio', 'device') + (g(s, 'audio', 'ok') === false ? ', not working' : '') : ''),
    vtx: (s) => { const t = g(s, 'vtx', 'text'); return typeof t === 'string' && t ? (t === 'off' ? 'Off' : t) : '–'; },
  };
  function onStatus(s) {
    for (const el of $$('[data-f]')) {
      const f = SF[el.dataset.f];
      if (!f) continue;
      let t;
      try { t = f(s); } catch (e) { t = '–'; }
      if (el.textContent !== t) el.textContent = t;
    }
    const n = s.network || {};
    $('#hostLine').textContent = (n.hostname ? n.hostname + '.local' : 'StickTime Pi') + (n.ip ? ' · ' + n.ip : '');
    $$('.hostUrl').forEach((el) => { el.textContent = hostUrl(); });
    $('#hotName').textContent = hostUrl();
    const err = g(s, 'game', 'error');
    $('#gameError').hidden = !err;
    if (err && $('#gameErrorText').textContent !== err) $('#gameErrorText').textContent = err;
    const list = $('#srcList'), srcs = g(s, 'input', 'sources') || [];
    const sig = JSON.stringify(srcs.map((x) => [x.kind, x.name, x.live, Math.round(x.rate_hz || 0)]));
    if (list.dataset.sig !== sig) {
      list.dataset.sig = sig;
      list.replaceChildren(...srcs.map((x, i) => h('li', {}, h('span', { class: 'livedot' + (x.live ? ' on' : ''), title: x.live ? 'moving' : 'idle' }),
        h('span', { text: (x.name || KIND[x.kind] || x.kind) + (i === g(s, 'input', 'active') ? ' (in use)' : '') }),
        h('span', { class: 'pill', text: KIND[x.kind] || x.kind }), h('span', { class: 'note', text: isNum(x.rate_hz) && x.rate_hz > 0 ? Math.round(x.rate_hz) + ' Hz' : '' }))));
      if (!srcs.length) list.append(h('li', { class: 'note', text: 'No stick source yet: plug in a radio, or start the ELRS module\'s WiFi joystick.' }));
    }
    if (page === 'sticks') $('#liveSrc').textContent = SF.inputName(s);
  }
  let statusT = 0;
  async function statusLoop() {
    clearTimeout(statusT);
    if (!document.hidden && !waiting) {
      try {
        lastStatus = await api('GET', '/api/status', undefined, { timeout: 5000 });
        onStatus(lastStatus);
      } catch (e) { /* the offline banner says it */ }
    }
    statusT = setTimeout(statusLoop, online === false ? 3000 : ['status', 'game', 'sound'].includes(page) ? 1000 : 4000);
  }
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) statusLoop();
    if (PAGES[page]) (document.hidden ? PAGES[page].leave : PAGES[page].enter)();
  });

  // ---------------------------------------------------------------- Status page
  function shot(img, empty) {
    const url = '/api/screenshot.png?t=' + Date.now();
    return new Promise((res) => {
      const pre = new Image();
      pre.onload = () => { img.src = url; img.hidden = false; empty.hidden = true; res(true); };
      pre.onerror = () => { empty.hidden = false; empty.textContent = 'No picture: the game isn\'t showing one.'; res(false); };
      pre.src = url;
    });
  }
  $('#shotBtn').addEventListener('click', async () => { const b = $('#shotBtn'); busy(b, true); await shot($('#shotImg'), $('#shotEmpty')); busy(b, false); });
  $$('[data-action="reload-game"]').forEach((b) => b.addEventListener('click', async () => {
    busy(b, true);
    try { await api('POST', '/api/game/reload'); toast('The game is reloading.'); } catch (e) { fail(e); }
    busy(b, false);
  }));

  // ---------------------------------------------------------------- Sticks page
  const AX = [['ail', 'Roll', 'Aileron'], ['ele', 'Pitch', 'Elevator'], ['thr', 'Throttle', 'Throttle'], ['rud', 'Yaw', 'Rudder']];
  function bar(name, title) {
    const fill = h('span', { class: 'fill' }), out = h('output', { text: '0' });
    const el = h('div', { class: 'bar', role: 'meter', 'aria-label': title || name, 'aria-valuemin': '-1024', 'aria-valuemax': '1024', 'aria-valuenow': '0' },
      h('span', { text: name }), h('span', { class: 'track' }, fill), out);
    let last = null;
    el.set = (v) => {
      v = isNum(v) ? Math.max(-1024, Math.min(1024, Math.round(v))) : 0;
      if (v === last) return;
      last = v;
      fill.style.transform = 'scaleX(' + (v / 1024) + ')';
      out.textContent = v;
      el.setAttribute('aria-valuenow', v);
    };
    return el;
  }
  const mix = {};
  $('#mixBars').append(...AX.map(([k, n, t]) => (mix[k] = bar(n, t))), (mix.pause = bar('Pause', 'Pause switch')));
  // which stick moves what, by the radio's stick mode (Mode 2: throttle and yaw on the left)
  const MODES = { 1: [['rud', 'ele'], ['ail', 'thr']], 2: [['rud', 'thr'], ['ail', 'ele']], 3: [['ail', 'ele'], ['rud', 'thr']], 4: [['ail', 'thr'], ['rud', 'ele']] };
  const NAMES = { ail: 'roll', ele: 'pitch', thr: 'throttle', rud: 'yaw' };
  function gimbalLabels() {
    const m = MODES[parseInt(S.values && S.values.stick_mode, 10)] || MODES[2];
    $('#gimL .glabel').textContent = NAMES[m[0][1]] + ' · ' + NAMES[m[0][0]];
    $('#gimR .glabel').textContent = NAMES[m[1][1]] + ' · ' + NAMES[m[1][0]];
  }
  document.addEventListener('settings', gimbalLabels);
  const srcCards = { sig: '', cards: [] };
  let sticksData = null, sticksRaf = 0;
  function drawSticks() {
    sticksRaf = 0;
    const d = sticksData;
    if (!d) return;
    const st = d.sticks || {};
    for (const k of ['ail', 'ele', 'thr', 'rud', 'pause']) mix[k].set(st[k]);
    const m = MODES[parseInt(S.values && S.values.stick_mode, 10)] || MODES[2], R = 55;
    const pos = (k) => (isNum(st[k]) ? Math.max(-1, Math.min(1, st[k] / 1024)) : 0);
    $('#gimL .gdot').style.transform = 'translate(' + pos(m[0][0]) * R + 'px,' + -pos(m[0][1]) * R + 'px)';
    $('#gimR .gdot').style.transform = 'translate(' + pos(m[1][0]) * R + 'px,' + -pos(m[1][1]) * R + 'px)';
    const srcs = Array.isArray(d.sources) ? d.sources : [];
    const sig = JSON.stringify(srcs.map((s) => [s.kind, s.name, s.nch]));
    if (sig !== srcCards.sig) {
      srcCards.sig = sig;
      srcCards.cards = srcs.map((s) => {
        const bars = [];
        for (let c = 0; c < 16; c++) bars.push(bar('CH' + (c + 1)));
        const dot = h('span', { class: 'livedot' }), rate = h('span', { class: 'note' }), active = h('span', { class: 'pill ok', text: 'in use', hidden: true });
        const card = h('section', { class: 'card src-card', 'aria-label': (s.name || s.kind) + ' channels' },
          h('div', { class: 'card-head' }, h('h2', {}, dot, ' ', s.name || KIND[s.kind] || s.kind, h('small', { text: KIND[s.kind] || s.kind })), h('span', {}, active, ' ', rate)),
          h('div', { class: 'bars' }, bars));
        return { card, bars, dot, rate, active };
      });
      $('#srcCards').replaceChildren(...srcCards.cards.map((c) => c.card));
      if (!srcs.length) $('#srcCards').append(h('section', { class: 'card' }, h('p', { class: 'note', text: 'No stick source yet. Plug in a radio by USB (as a joystick), start the ELRS or Crossfire module\'s WiFi, or pair a Bluetooth gamepad.' })));
    }
    srcs.forEach((s, i) => {
      const c = srcCards.cards[i];
      if (!c) return;
      const ch = Array.isArray(s.ch) ? s.ch : [];
      c.bars.forEach((b, j) => { b.set(ch[j]); b.classList.toggle('off', j >= (s.nch || ch.length)); });
      c.dot.className = 'livedot' + (s.live ? ' on' : '');
      c.rate.textContent = isNum(s.rate_hz) && s.rate_hz > 0 ? Math.round(s.rate_hz) + ' Hz' : s.live ? '' : 'idle';
      c.active.hidden = i !== d.active;
    });
  }
  const sticksFeed = {
    es: null, poll: 0, on: false, errors: 0,
    start() {
      this.on = true;
      this.errors = 0;
      if (typeof EventSource === 'function' && store.get('nosse') !== '1') this.sse();
      else this.pollLoop();
    },
    sse() {
      const es = this.es = new EventSource('/api/sticks/stream');
      es.onmessage = (e) => { this.errors = 0; this.got(e.data); };
      es.onerror = () => {
        // a dropped stream: once more, then polling (20 Hz)
        es.close();
        if (this.es !== es || !this.on) return;
        this.es = null;
        if (++this.errors < 2) setTimeout(() => this.on && !this.es && this.sse(), 1000);
        else this.pollLoop();
      };
    },
    got(text) {
      try { sticksData = typeof text === 'string' ? JSON.parse(text) : text; } catch (e) { return; }
      if (!sticksRaf) sticksRaf = requestAnimationFrame(drawSticks);
    },
    async pollLoop() {
      while (this.on && !this.es) {
        const t = Date.now();
        try { this.got(await api('GET', '/api/sticks', undefined, { timeout: 3000 })); } catch (e) { await sleep(1000); }
        await sleep(Math.max(0, 50 - (Date.now() - t)));
      }
    },
    stop() {
      this.on = false;
      if (this.es) { this.es.close(); this.es = null; }
    },
  };
  $$('[data-modap]').forEach((b) => b.addEventListener('click', async () => {
    const m = b.dataset.modap, name = m === 'elrs' ? 'ELRS' : 'TBS';
    if (!await ask('Join the ' + name + ' module\'s WiFi?', 'The Pi leaves this network and joins the module\'s own WiFi. This page loses the Pi: join the module\'s network with this device too, then open ' + hostUrl() + '.', 'Join')) return;
    busy(b, true);
    try { await api('POST', '/api/wifi/module-ap', m); toast('The Pi is joining the ' + name + ' module\'s WiFi.'); } catch (e) { fail(e); }
    busy(b, false);
  }));

  // ---------------------------------------------------------------- WiFi page
  async function scan() {
    const btn = $('#scanBtn'), note = $('#scanNote');
    busy(btn, true);
    note.textContent = 'Scanning…';
    let j = null;
    for (let i = 0; i < 12 && !j; i++) {
      try { j = await api('GET', '/api/wifi/scan', undefined, { timeout: 20000 }); } catch (e) {
        if (e.status === 503 || e.name === 'Busy') { await sleep(1500); continue; }
        note.textContent = 'Scan failed: ' + e.message;
        busy(btn, false);
        return;
      }
    }
    busy(btn, false);
    if (!j) { note.textContent = 'The scan takes long: try again in a moment.'; return; }
    showNets(j, '');
  }
  // Opening the tab shows the last scan: a new scan takes the Pi's WiFi radio off its channel for a
  // moment, and that interrupts the WiFi joystick. Only the Scan button scans.
  async function lastScan() {
    const note = $('#scanNote');
    let j = null;
    try { j = await api('GET', '/api/wifi/scan?cached=1'); } catch (e) { j = null; }
    if (j && Array.isArray(j.networks) && j.networks.length) showNets(j, 'Last scan. ');
    else note.textContent = 'Press Scan to list the networks the Pi can see. The WiFi joystick pauses for a moment while the Pi scans.';
  }
  function showNets(j, prefix) {
    const note = $('#scanNote');
    const nets = (Array.isArray(j.networks) ? j.networks : []).slice().sort((a, b) => (b.known - a.known) || ((b.signal || 0) - (a.signal || 0)));
    note.textContent = nets.length ? prefix + nets.length + ' networks' + (j.at ? ', ' + when(j.at) : '') + '. Tap one to join it.' : 'No networks found.';
    $('#netList').replaceChildren(...nets.map((n) => {
      const bars = n.signal >= 75 ? 4 : n.signal >= 50 ? 3 : n.signal >= 25 ? 2 : n.signal > 0 ? 1 : 0;
      const meta = [n.band ? n.band + ' GHz' : '', n.chan ? 'ch ' + n.chan : '', n.security ? n.security : 'open', n.known ? 'saved' : ''].filter(Boolean).join(' · ');
      return h('li', {}, h('button', { class: 'net', type: 'button', 'aria-label': (n.ssid || 'Hidden network') + ', signal ' + (n.signal || 0) + '%, ' + meta, on: { click: () => join(n) } },
        h('span', { class: 'sig', 'data-n': String(bars), 'aria-hidden': 'true' }, h('i'), h('i'), h('i'), h('i')),
        h('span', { class: 'ssid', text: n.ssid || '(hidden network)' }), n.security ? icon('lock') : null, h('span', { class: 'meta', text: meta })));
    }));
  }
  $('#scanBtn').addEventListener('click', scan);
  function join(n) {
    const dlg = $('#joinDlg');
    $('#joinSsid').value = n ? n.ssid || '' : '';
    $('#joinSsid').readOnly = !!(n && n.ssid);
    $('#joinPass').value = '';
    $('#joinPass').type = 'password';
    $('#joinPassBox').hidden = !!(n && n.ssid && !n.security);
    $('#joinCountry').value = (S.values && S.values.wifi_country) || '';
    $('#joinHidden').checked = !n;
    $('#joinTitle').textContent = n && n.ssid ? 'Join ' + n.ssid : 'Join a hidden network';
    for (const e of ['joinSsidErr', 'joinPassErr', 'joinCountryErr', 'joinErr']) $('#' + e).textContent = '';
    if (typeof dlg.showModal === 'function') dlg.showModal(); else dlg.setAttribute('open', '');
    (n && n.ssid ? ($('#joinPassBox').hidden ? $('#joinGo') : $('#joinPass')) : $('#joinSsid')).focus();
  }
  $('#otherNet').addEventListener('click', () => join(null));
  $('#joinCancel').addEventListener('click', () => $('#joinDlg').close());
  $$('[data-reveal]').forEach((b) => b.addEventListener('click', () => {
    const inp = $('#' + b.dataset.reveal), show = inp.type === 'password';
    inp.type = show ? 'text' : 'password';
    b.textContent = show ? 'Hide' : 'Show';
    b.setAttribute('aria-pressed', String(show));
  }));
  $('#joinForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const ssid = $('#joinSsid').value, pass = $('#joinPassBox').hidden ? '' : $('#joinPass').value, country = $('#joinCountry').value.trim().toUpperCase();
    const errs = {};
    if (!ssid || utf8len(ssid) > 32) errs.joinSsidErr = ssid ? '32 bytes at most' : 'The network\'s name';
    if (/[\x00-\x1f\x7f]/.test(ssid)) errs.joinSsidErr = 'No line breaks';
    if (pass && !/^[0-9A-Fa-f]{64}$/.test(pass) && (utf8len(pass) < 8 || utf8len(pass) > 63)) errs.joinPassErr = 'WiFi passwords have 8 to 63 characters (or are 64 hex digits)';
    if (/[\x00-\x1f\x7f]/.test(pass)) errs.joinPassErr = 'No line breaks';
    if (country && !/^[A-Z]{2}$/.test(country)) errs.joinCountryErr = 'Two letters, like US or DE';
    for (const k of ['joinSsidErr', 'joinPassErr', 'joinCountryErr', 'joinErr']) $('#' + k).textContent = errs[k] || '';
    if (Object.keys(errs).length) return;
    const body = [line('ssid', ssid), line('password', pass)];
    if (country) body.push(line('country', country));
    if ($('#joinHidden').checked) body.push('hidden = 1');
    const go = $('#joinGo');
    busy(go, true);
    try {
      await api('POST', '/api/wifi', body.join('\n') + '\n', { timeout: 15000 });
    } catch (err) {
      busy(go, false);
      if (err.fields) {
        const map = { ssid: 'joinSsidErr', password: 'joinPassErr', country: 'joinCountryErr' };
        for (const [k, m] of Object.entries(err.fields)) $('#' + (map[k] || 'joinErr')).textContent = m;
      } else $('#joinErr').textContent = err.message;
      return;
    }
    busy(go, false);
    $('#joinDlg').close();
    if (S.values) { S.values.wifi_ssid = ssid; if (country) S.values.wifi_country = country; }
    watchJoin(ssid);
  });
  // after Join: the Pi switches networks in the background; say what is going on
  async function watchJoin(ssid) {
    const box = $('#joinState');
    box.hidden = false;
    box.className = 'alert info';
    const say = (t, kind) => { box.className = 'alert ' + (kind || 'info'); box.textContent = t; };
    say('Joining "' + ssid + '"… the Pi may drop off this network for a moment.');
    const t0 = Date.now();
    let lost = false;
    while (Date.now() - t0 < 90000) {
      await sleep(3000);
      try {
        const s = await api('GET', '/api/status', undefined, { timeout: 4000, quiet: true });
        const n = s.network || {};
        if (n.ssid === ssid && n.mode !== 'hotspot') { say('Connected to "' + ssid + '"' + (n.ip ? ' as ' + n.ip : '') + '.', 'ok'); return; }
        if (lost) { say('The Pi is back, on ' + (n.mode === 'hotspot' ? 'its own hotspot: it couldn\'t join "' + ssid + '". Check the password.' : '"' + (n.ssid || '?') + '".'), 'warn'); return; }
      } catch (e) {
        lost = true;
        say('The Pi left this network to join "' + ssid + '". Join "' + ssid + '" with this device too, then open ' + hostUrl() + ' (or the address on the goggles\' main menu). If it can\'t join, it makes its hotspot again.', 'warn');
      }
    }
  }

  // ---------------------------------------------------------------- Sound page
  const SONG_EXT = /\.(mp3|ogg|wav)$/i;
  // a file name the Pi takes (the SD rules of WEB_API.md, 120 bytes; no dot files, no DOS device
  // names: the card is read on PCs too)
  function songName(name) {
    let n = String(name).normalize('NFC').replace(/[\x00-\x1f\x7f"*:<>?|\\/]/g, '_').replace(/^[ .]+/, '');
    const m = /\.[A-Za-z0-9]+$/.exec(n), ext = m ? m[0].toLowerCase() : '';
    let base = ext ? n.slice(0, -ext.length) : n;
    while (utf8len(base + ext) > 120 && base) base = Array.from(base).slice(0, -1).join('');   // whole characters
    base = base.replace(/[ .]+$/, '');
    base = base.replace(/^(con|prn|aux|nul|com[0-9]|lpt[0-9])(?=\.|$)/i, '$1_');      // the part before the first dot
    return (base || 'song') + ext;
  }
  let songs = [];
  async function loadSongs() {
    let j;
    try { j = await api('GET', '/api/songs'); } catch (e) {
      $('#songList').replaceChildren(h('li', { class: 'empty', text: 'Couldn\'t list the songs: ' + e.message }));
      return;
    }
    songs = Array.isArray(j.songs) ? j.songs : [];
    $('#songFree').textContent = isNum(j.free_mb) && j.free_mb >= 0 ? j.free_mb + ' MB free' : '';
    $('#songList').replaceChildren(...(songs.length ? songs.map((s) => h('li', {},
      h('span', { class: 'name', text: s.name }), h('span', { class: 'size', text: bytes(s.size) }),
      h('button', { class: 'btn sm ghost', type: 'button', 'aria-label': 'Delete ' + s.name, title: 'Delete', on: { click: (e) => delSong(s.name, e.currentTarget) } }, icon('trash'))))
      : [h('li', { class: 'empty', text: 'No songs yet. Add some: with Music on and set to My songs, they play in order.' })]));
  }
  async function delSong(name, btn) {
    if (!await ask('Delete ' + name + '?', 'It is removed from the Pi\'s music folder.', 'Delete', true)) return;
    busy(btn, true);
    try { await api('DELETE', '/api/songs/' + encodeURIComponent(name)); toast('Deleted ' + name + '.'); } catch (e) { fail(e); }
    loadSongs();
  }
  async function addSongs(files) {
    files = Array.from(files || []);
    if (!files.length) return;
    const list = $('#uploads'), plan = [];
    for (const f of files) {
      const name = songName(f.name);
      const row = { f, name, st: h('span', { class: 'st' }), bar: h('progress', { max: 1, value: 0 }) };
      row.li = h('li', {}, h('span', { class: 'name', text: name + (name !== f.name ? ' (was ' + f.name + ')' : '') }), row.st, row.bar);
      list.append(row.li);
      if (!SONG_EXT.test(name)) { row.st.textContent = 'not an MP3, OGG or WAV file'; row.st.className = 'st bad'; row.bar.hidden = true; continue; }
      if (f.size > 64 * 1048576) { row.st.textContent = 'over 64 MB'; row.st.className = 'st bad'; row.bar.hidden = true; continue; }
      if (plan.some((p) => p.name.toLowerCase() === name.toLowerCase())) { row.st.textContent = 'twice in this batch'; row.st.className = 'st bad'; row.bar.hidden = true; continue; }
      row.st.textContent = 'waiting';
      plan.push(row);
    }
    const dup = plan.filter((p) => songs.some((s) => s.name.toLowerCase() === p.name.toLowerCase()));
    if (dup.length && !await ask('Replace ' + (dup.length === 1 ? dup[0].name : dup.length + ' songs') + '?', 'The Pi has ' + (dup.length === 1 ? 'a song' : 'songs') + ' with that name already: ' + dup.map((d) => d.name).join(', ') + '.', 'Replace')) {
      for (const d of dup) { d.st.textContent = 'skipped'; d.bar.hidden = true; plan.splice(plan.indexOf(d), 1); }
    }
    let ok = 0;
    for (const row of plan) {
      row.st.textContent = '0%';
      const up = upload('/api/songs/' + encodeURIComponent(row.name), row.f, (p) => { row.bar.value = p; row.st.textContent = Math.round(p * 100) + '%'; });
      try {
        await up.done;
        row.bar.value = 1;
        row.st.textContent = 'done';
        row.st.className = 'st ok';
        ok++;
      } catch (e) {
        row.st.textContent = e.message;
        row.st.className = 'st bad';
      }
    }
    if (ok) toast(ok === 1 ? 'Song added.' : ok + ' songs added.');
    setTimeout(() => { for (const li of $$('li', list)) if ($('.st.ok', li)) li.remove(); }, 4000);
    loadSongs();
  }
  $('#songInput').addEventListener('change', (e) => { addSongs(e.target.files); e.target.value = ''; });
  const drop = $('#songDrop');
  drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
  drop.addEventListener('dragleave', () => drop.classList.remove('over'));
  drop.addEventListener('drop', (e) => { e.preventDefault(); drop.classList.remove('over'); addSongs(e.dataTransfer && e.dataTransfer.files); });
  $('#nextSong').addEventListener('click', async () => {
    const b = $('#nextSong');
    busy(b, true);
    try { await api('POST', '/api/music/next'); toast('Next song.'); setTimeout(statusLoop, 300); } catch (e) { fail(e); }
    busy(b, false);
  });

  // ---------------------------------------------------------------- Tracks page
  const TRACKS = 'SCRIPTS/TOOLS/StickTimeTracks';
  async function loadSlots() {
    const ol = $('#slotList');
    let ents = [];
    try { ents = (await api('GET', '/api/sd/' + TRACKS + '?list')).entries || []; } catch (e) {
      if (e.status !== 404) { ol.replaceChildren(h('li', { class: 'note', text: 'Couldn\'t read the Pi\'s tracks: ' + e.message })); return; }
    }
    const rows = await Promise.all([1, 2, 3, 4, 5, 6, 7, 8].map(async (n) => {
      const e = ents.find((x) => x.kind === 'file' && String(x.name).toLowerCase() === 'c' + n + '.txt');
      let name = null, bad = '';
      if (e) {
        if (e.size > 32768) bad = 'too big to be a track';
        else {
          try {
            const t = await api('GET', '/api/sd/' + TRACKS + '/' + encodeURIComponent(e.name));
            const m = /^\s*([^\r\n]+)[\r\n]+\s*(\d+)\s+\d+/.exec(String(t));
            if (m && m[2] === '1') name = m[1].trim().slice(0, 16); else bad = 'the game can\'t read it';
          } catch (err) { bad = err.message; }
        }
      }
      return h('li', {}, h('span', { class: 'no', text: 'c' + n }),
        h('span', { class: 'tname' + (name ? '' : ' empty'), text: name || (e ? bad : 'empty') }),
        h('span', { class: 'tno', text: 'track ' + (8 + n) }),
        e ? h('a', { class: 'btn sm ghost', href: '/api/sd/' + TRACKS + '/' + encodeURIComponent(e.name), download: 'c' + n + '.txt', 'aria-label': 'Download c' + n + '.txt' }, 'Download') : null);
    }));
    ol.replaceChildren(...rows);
  }
  $('#slotsBtn').addEventListener('click', loadSlots);

  // ---------------------------------------------------------------- Game page
  let shotBusy = false, liveT = 0;
  async function remoteShot() {
    if (shotBusy) return;
    shotBusy = true;
    await shot($('#remoteImg'), $('#remoteEmpty'));
    shotBusy = false;
  }
  let keyShotT = 0;
  async function sendKey(k, btn) {
    if (btn) { btn.classList.add('down'); setTimeout(() => btn.classList.remove('down'), 120); }
    if (navigator.vibrate) try { navigator.vibrate(8); } catch (e) { /* not allowed */ }
    try { await api('POST', '/api/key', k, { timeout: 4000 }); } catch (e) { fail(e); return; }
    if (!$('#liveView').checked) { clearTimeout(keyShotT); keyShotT = setTimeout(remoteShot, 250); }
  }
  $$('.keypad [data-key]').forEach((b) => b.addEventListener('click', () => sendKey(b.dataset.key, b)));
  const KEYMAP = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'prev', ArrowRight: 'next', Enter: 'enter', Escape: 'exit', Backspace: 'exit',
    '+': 'inc', '=': 'inc', '-': 'dec', PageUp: 'prevpage', PageDown: 'nextpage', m: 'menu', M: 'menu' };
  document.addEventListener('keydown', (e) => {
    if (page !== 'game' || e.ctrlKey || e.metaKey || e.altKey || document.querySelector('dialog[open]')) return;
    const t = e.target;
    if (t && (/^(INPUT|SELECT|TEXTAREA)$/.test(t.tagName) || t.isContentEditable)) return;
    const k = KEYMAP[e.key];
    if (!k) return;
    if (e.key === 'Enter' && t && t.closest && t.closest('button, a')) return;   // a focused button's own Enter
    e.preventDefault();
    sendKey(k, $('.keypad [data-key="' + k + '"]'));
  });
  function liveLoop() {
    clearTimeout(liveT);
    if (page !== 'game' || !$('#liveView').checked || document.hidden) return;
    remoteShot().then(() => { liveT = setTimeout(liveLoop, 350); });
  }
  $('#liveView').addEventListener('change', liveLoop);
  async function loadTools() {
    const box = $('#toolList');
    let j;
    try { j = await api('GET', '/api/tools'); } catch (e) { box.replaceChildren(h('p', { class: 'note', text: 'Couldn\'t list the scripts: ' + e.message })); return; }
    const tools = Array.isArray(j.tools) ? j.tools : [];
    const cur = String(j.current || (S.values && S.values.script) || '').toLowerCase();
    if (!tools.length) { box.replaceChildren(h('p', { class: 'note', text: 'No tool scripts in /SCRIPTS/TOOLS. Install StickTime from the Tracks page (Install StickTime).' })); return; }
    box.replaceChildren(...tools.map((t) => {
      const r = h('input', { type: 'radio', name: 'tool', value: t.path, checked: String(t.path).toLowerCase() === cur });
      r.addEventListener('change', async () => {
        try {
          const res = await api('POST', '/api/settings', line('script', t.path) + '\n');
          if (S.values) S.values.script = t.path;
          toast('Running ' + (t.name || t.path) + '.');
          if (res && res.reboot) $('#rebootBanner').hidden = false;
        } catch (e) { fail(e); loadTools(); }
      });
      return h('label', { class: 'tool' }, r, h('span', { class: 'tcol' }, h('b', { text: t.name || t.path }), h('span', { class: 'tpath', text: t.path })));
    }));
  }

  // ---------------------------------------------------------------- Updates page
  let upd = null, updT = 0;
  const BUSY_STATES = new Set(['checking', 'downloading', 'verifying', 'installing']);
  // after a POST the updater is started in the background: until it writes its state, the file may
  // still hold the last run's (done, error), so that isn't taken as the answer for a few seconds
  let updWatch = 0, updSeen = true;
  function expectUpdater() { updWatch = Date.now() + 8000; updSeen = false; }
  const updFinal = () => updSeen || Date.now() > updWatch;
  function notes(el, text) {
    el.replaceChildren();
    el.hidden = !text;
    if (!text) return;
    let ul = null;
    for (const raw of String(text).split(/\r?\n/)) {
      const t = raw.trim();
      if (!t) { ul = null; continue; }
      const hd = /^#{1,6}\s+(.*)$/.exec(t), li = /^[-*]\s+(.*)$/.exec(t);
      if (hd) { ul = null; el.append(h('h3', { text: hd[1] })); } else if (li) {
        if (!ul) { ul = h('ul'); el.append(ul); }
        ul.append(h('li', { text: li[1] }));
      } else { ul = null; el.append(h('p', { text: t })); }
    }
  }
  function renderUpdate(u) {
    upd = u || {};
    const state = String(upd.state || 'idle');
    if (BUSY_STATES.has(state)) updSeen = true;
    const starting = !updFinal(), active = BUSY_STATES.has(state) || starting;
    const err = typeof upd.error === 'string' ? upd.error : upd.error && upd.error.message ? upd.error.message : '';
    $('#updMsg').textContent = starting ? 'Starting the updater…' : upd.message || ({ idle: 'Up to date as far as the Pi knows.', checking: 'Checking…', downloading: 'Downloading…', installing: 'Installing…', done: 'Done.', error: 'Something went wrong.' })[state] || state;
    const bar = $('#updBar');
    bar.hidden = !active;
    if (isNum(upd.progress) && upd.progress >= 0 && upd.progress <= 1) bar.value = upd.progress; else bar.removeAttribute('value');
    $('#updChecked').textContent = upd.checked_at ? 'Last checked ' + when(upd.checked_at) + '.' : 'Not checked yet.';
    $('#updErr').hidden = starting || (!err && state !== 'error');
    $('#updErr').textContent = err || (state === 'error' ? upd.message || 'The update failed.' : '');
    busy($('#updCheck'), active);
    let any = false;
    for (const what of ['game', 'app']) {
      const card = $(what === 'game' ? '#updGame' : '#updApp'), x = upd[what] || {};
      const inst = x.installed || x.version || '', latest = x.latest || '', staged = x.staged || '';
      const newer = (latest && cmpVer(latest, inst) > 0) || (staged && cmpVer(staged, inst) > 0);
      any = any || !!newer;
      for (const el of $$('[data-u]', card)) {
        const k = el.dataset.u;
        if (k === 'notes') notes(el, x.notes);
        else if (k === 'staged') el.textContent = staged ? staged + (staged === inst ? ' (installed)' : ' (ready for Install)') : '–';
        else if (k === 'installed') el.textContent = (inst || '–') + (x.custom ? ' (your own copy)' : '');
        else el.textContent = latest ? latest + (cmpVer(latest, inst) > 0 ? ' (new)' : '') : '–';
      }
      const ap = $('[data-apply]', card), rb = $('[data-rollback]', card);
      ap.textContent = newer ? 'Install ' + (cmpVer(staged, latest) > 0 ? staged : latest) : 'Up to date';
      if (!active) { ap.classList.remove('busy'); ap.removeAttribute('aria-busy'); }
      ap.disabled = active || !newer;
      rb.disabled = active || x.can_rollback === false;
    }
    $('#updBadge').hidden = !any;
    clearTimeout(updT);
    if (active) updT = setTimeout(loadUpdate, 1000);
  }
  async function loadUpdate(quiet) {
    try { renderUpdate(await api('GET', '/api/update', undefined, { quiet: !!quiet })); } catch (e) {
      if (quiet) return;
      $('#updMsg').textContent = 'Couldn\'t read the updater\'s state: ' + e.message;
    }
  }
  $('#updCheck').addEventListener('click', async () => {
    try { await api('POST', '/api/update/check'); } catch (e) { fail(e); return; }
    expectUpdater();
    renderUpdate(upd);
    setTimeout(loadUpdate, 600);
  });
  $$('[data-apply]').forEach((b) => b.addEventListener('click', async () => {
    const what = b.dataset.apply, x = (upd && upd[what]) || {};
    const v = x.staged && cmpVer(x.staged, x.latest) > 0 ? x.staged : x.latest;
    const ok = what === 'game'
      ? await ask('Install StickTime ' + (v || '') + '?', 'The game is replaced when it is back in its menus. Your tracks, ghosts and best times stay (they are backed up first).', 'Install')
      : await ask('Install StickTime Pi ' + (v || '') + '?', 'The game and this page restart; the page comes back by itself. Roll back undoes it.', 'Install');
    if (!ok) return;
    busy(b, true);
    const sent = Date.now();
    try { await api('POST', '/api/update/apply', what); } catch (e) { busy(b, false); fail(e); return; }
    expectUpdater();
    if (what === 'app') { appUpdate(sent); return; }
    setTimeout(loadUpdate, 500);
  }));
  // the app update replaces this page's files and restarts the service: wait, then load the new page
  async function appUpdate(sent) {
    const from = lastStatus && lastStatus.version;
    const t0 = Date.now();
    while (Date.now() - t0 < 600000) {
      let u = null;
      try { u = await api('GET', '/api/update', undefined, { quiet: true, timeout: 4000 }); } catch (e) { break; }   // gone: restarting
      renderUpdate(u);
      if (updFinal() && u.state === 'error') return;
      if (updFinal() && u.state === 'done') break;
      await sleep(1000);
    }
    const back = await waitForPi('Installing StickTime Pi…', 'The game restarts with the new version. This page reloads when it is back.', { timeout: 180000, assume: 15000, since: sent });
    if (back && back.version !== from) location.reload();
    else if (back) loadUpdate();
  }
  $$('[data-rollback]').forEach((b) => b.addEventListener('click', async () => {
    const what = b.dataset.rollback;
    if (!await ask('Roll back ' + (what === 'game' ? 'the game' : 'StickTime Pi') + '?', 'The version before the last update comes back.', 'Roll back')) return;
    const sent = Date.now();
    try { await api('POST', '/api/update/rollback', what); } catch (e) { fail(e); return; }
    expectUpdater();
    if (what === 'app') { appUpdate(sent); return; }
    setTimeout(loadUpdate, 500);
  }));

  // ---------------------------------------------------------------- System page
  async function loadSystem() {
    try {
      const s = await api('GET', '/api/system');
      const d = s.disk || {};
      const v = { model: str(s.model), os: str(s.os), kernel: str(s.kernel), disk: isNum(d.free_mb) ? d.free_mb + ' MB free' + (isNum(d.total_mb) ? ' of ' + d.total_mb + ' MB' : '') : '–' };
      for (const el of $$('[data-s]')) el.textContent = v[el.dataset.s];
    } catch (e) { /* the offline banner says it */ }
  }
  let logT = 0;
  async function loadLog() {
    const pre = $('#logText');
    const atEnd = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 24;
    try { pre.textContent = (await api('GET', '/api/log')) || '(empty)'; } catch (e) { pre.textContent = 'Couldn\'t read the log: ' + e.message; }
    if (atEnd || pre.dataset.first !== '1') { pre.scrollTop = pre.scrollHeight; pre.dataset.first = '1'; }
    clearTimeout(logT);
    if ($('#logAuto').checked && page === 'system') logT = setTimeout(loadLog, 2000);
  }
  $('#logBtn').addEventListener('click', loadLog);
  $('#logAuto').addEventListener('change', loadLog);
  $('#logSave').addEventListener('click', () => download('sticktime-log.txt', $('#logText').textContent));

  // ---------------------------------------------------------------- Video page: VTX control
  // GET /api/vtx while the page is open (the VTX thread's view: what it found, what the VTX is set to,
  // the table); the pickers post to /api/vtx at once (saved like settings, applied by the VTX thread)
  const V = { data: null, t: 0, fast: 0, sig: '', posting: false };
  const VTX_STATE = { off: ['info', '', 'Off'], no_port: ['warn', 'warn', 'UART off'], searching: ['info', '', 'Searching'],
    not_found: ['warn', 'warn', 'Not found'], syncing: ['info', '', 'Setting'], ready: ['ok', 'ok', 'Ready'],
    send_only: ['warn', 'warn', 'Send-only'], error: ['bad', 'bad', 'Problem'] };
  const mw = (label) => (/^\d+$/.test(label) ? label + ' mW' : label);
  const vtxProto = (v) => (v.protocol === 'smartaudio' ? 'SmartAudio' + (v.version ? ' ' + v.version : '') : v.protocol === 'tramp' ? 'Tramp' : '');
  const vtxBands = (v) => (v && v.table && Array.isArray(v.table.bands) ? v.table.bands.filter((b) => b && typeof b.letter === 'string') : []);
  const vtxPowers = (v) => (v && v.table && Array.isArray(v.table.powers) ? v.table.powers.filter((p) => p && typeof p === 'object') : []);
  function vtxRender(v) {
    if (!v || typeof v !== 'object' || typeof v.state !== 'string') return;
    V.data = v;
    const st = VTX_STATE[v.state] || ['bad', 'bad', v.state];
    const pill = $('#vtxPill');
    pill.textContent = st[2];
    pill.className = 'pill' + (st[1] ? ' ' + st[1] : '');
    const box = $('#vtxMsg');
    box.className = 'alert ' + st[0];
    box.querySelector('use').setAttribute('href', st[0] === 'ok' ? '#i-check' : st[0] === 'info' ? '#i-video' : '#i-warn');
    let msg = typeof v.message === 'string' ? v.message : '';
    if (v.state === 'off') msg = 'VTX control is off. Wire the VTX, then turn it on below (Auto finds SmartAudio or Tramp).';
    if (typeof v.error === 'string' && v.error && !msg.includes(v.error)) msg += ' ' + v.error;
    $('#vtxMsgText').textContent = msg;
    $('#vtxReboot').hidden = !v.needs_reboot;
    $('#vtxLive').hidden = !v.enabled;
    if (!v.enabled) return;
    const tbl = v.table && typeof v.table === 'object' ? v.table : {}, bands = vtxBands(v), powers = vtxPowers(v);
    $('#vtxDev').textContent = [vtxProto(v), v.line, v.device].filter((x) => typeof x === 'string' && x).join(' · ') || '–';
    $('#vtxNow').textContent = v.found ? (v.band ? v.band + v.channel + ' · ' : '') + v.freq + ' MHz' + (v.locked ? ' (locked)' : '')
      : v.send_only ? 'not read back (send-only)' : '–';
    const p = v.power && typeof v.power === 'object' ? v.power : null;
    $('#vtxPowNow').textContent = v.found && p ? (p.label ? mw(p.label) : isNum(p.mw) && p.mw ? p.mw + ' mW' : '?') +
      (v.pit ? ', pit mode' : v.pit_at_boot ? ', pit mode at the next power-up' : '') : '–';
    const hasT = isNum(v.temp_c);
    $('#vtxTempK').hidden = $('#vtxTemp').hidden = !hasT;
    if (hasT) $('#vtxTemp').textContent = v.temp_c + ' °C';
    const ls = typeof v.line_stats === 'string' ? v.line_stats : '';      // the single pin's TX timing
    $('#vtxStatsK').hidden = $('#vtxStats').hidden = !ls;
    if (ls) $('#vtxStats').textContent = ls;
    $('#vtxTableLine').textContent = 'VTX table: ' + (tbl.source === 'file' ? 'loaded (' + (tbl.description || 'no description') + ')' : 'built in' +
      (tbl.description ? ' (' + tbl.description.replace(/^StickTime Pi default: /, '') + ')' : '')) + ', ' + bands.length + ' bands, ' +
      powers.length + ' power levels.' + (tbl.error ? ' ' + tbl.error : '');
    $('#vtxDefault').disabled = tbl.source !== 'file';
    const sig = JSON.stringify([bands, powers, tbl.channels, v.settings]);
    if (sig !== V.sig && !V.posting) { V.sig = sig; vtxPickers(v); }
    vtxMarkNow(v);
  }
  function vtxChannels(v, letter) {
    const set = v.settings || {}, b = vtxBands(v).find((x) => x.letter === letter);
    const n = isNum(v.table && v.table.channels) && v.table.channels > 0 ? Math.min(v.table.channels, 8) : 8, kids = [];
    for (let c = 1; c <= n; c++) {
      const f = b && Array.isArray(b.frequencies) && isNum(b.frequencies[c - 1]) ? b.frequencies[c - 1] : 0;
      kids.push(h('label', {}, h('input', { type: 'radio', name: 'vtxch', value: String(c), disabled: !f, checked: !!f && set.band === letter && !set.freq && set.channel === c,
        'aria-label': 'Channel ' + c + (f ? ', ' + f + ' MHz' : b ? ', not in the table' : ''), on: { change: () => vtxPost('band = ' + letter + '\nchannel = ' + c + '\n') } }),
      h('span', {}, String(c), h('small', { text: f ? String(f) : '–' }))));
    }
    $('#vtxChan').replaceChildren(...kids);
  }
  function vtxPickers(v) {
    const set = v.settings || {}, bands = vtxBands(v), sel = $('#vtxBand');
    sel.replaceChildren(h('option', { value: '', text: '– leave it –' }), ...bands.map((b) => h('option', { value: b.letter, text: b.letter + ' · ' + (b.name || '') })));
    sel.value = bands.some((b) => b.letter === set.band) ? set.band : '';
    vtxChannels(v, sel.value);
    $('#vtxPower').replaceChildren(...vtxPowers(v).map((p, i) => h('label', {},
      h('input', { type: 'radio', name: 'vtxpow', value: String(i + 1), checked: set.power === i + 1, on: { change: () => vtxPost('power = ' + (i + 1) + '\n') } }),
      h('span', {}, mw(typeof p.label === 'string' && p.label ? p.label : String(p.value)), h('small', { text: 'level ' + (i + 1) })))));
    $('#vtxPit').checked = !!set.pit;
    const fi = $('#vtxFreq');
    if (document.activeElement !== fi) fi.value = set.freq ? String(set.freq) : '';
    $('#vtxFreqClear').disabled = !set.freq;
  }
  // a green dot where the VTX is now
  function vtxMarkNow(v) {
    for (const s of $$('#vtxChan span, #vtxPower span')) { s.classList.remove('now'); s.removeAttribute('title'); }
    if (!v.found) return;
    const mark = (span) => { if (span) { span.classList.add('now'); span.title = 'The VTX is here now'; } };
    if (v.band && v.band === $('#vtxBand').value && v.channel) mark($(`#vtxChan input[value="${v.channel}"] + span`));
    if (v.power && isNum(v.power.level) && v.power.level > 0) mark($(`#vtxPower input[value="${v.power.level}"] + span`));
  }
  async function vtxPost(body) {
    V.posting = true;
    try {
      await api('POST', '/api/vtx', body);
      V.fast = Date.now() + 6000;
    } catch (e) {
      if (e.fields) toast('Not set: ' + Object.entries(e.fields).map(([k, m]) => k + ' ' + m).join('; '), 'bad');
      else fail(e);
    }
    V.posting = false;
    V.sig = '';                                    // the pickers as the Pi has it now
    vtxLoop();
  }
  async function vtxLoop() {
    clearTimeout(V.t);
    if (page !== 'video') return;
    if (!document.hidden) {
      try { vtxRender(await api('GET', '/api/vtx', undefined, { quiet: true, timeout: 5000 })); } catch (e) { /* the offline banner says it */ }
    }
    V.t = setTimeout(vtxLoop, Date.now() < V.fast ? 400 : V.data && V.data.state === 'ready' ? 2000 : 1000);
  }
  $('#vtxBand').addEventListener('change', () => {
    const v = V.data, letter = $('#vtxBand').value;
    if (!v) return;
    if (!letter) { vtxPost('band = \n'); return; }
    const b = vtxBands(v).find((x) => x.letter === letter), fr = b && Array.isArray(b.frequencies) ? b.frequencies : [];
    let c = (v.settings && v.settings.channel) || 1;
    if (!fr[c - 1]) c = fr.findIndex((f) => f > 0) + 1 || 1;
    vtxChannels(v, letter);
    vtxPost('band = ' + letter + '\nchannel = ' + c + '\n');
  });
  $('#vtxPit').addEventListener('change', () => vtxPost('pit = ' + ($('#vtxPit').checked ? 'on' : 'off') + '\n'));
  $('#vtxFreqSet').addEventListener('click', () => {
    const t = $('#vtxFreq').value.trim(), n = Number(t);
    $('#vtxFreqErr').textContent = '';
    if (!/^\d{4}$/.test(t) || n < 4900 || n > 6200) { $('#vtxFreqErr').textContent = 'A frequency from 4900 to 6200 MHz'; $('#vtxFreq').focus(); return; }
    vtxPost('freq = ' + n + '\n');
  });
  $('#vtxFreq').addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); $('#vtxFreqSet').click(); } });
  $('#vtxFreqClear').addEventListener('click', () => { $('#vtxFreq').value = ''; $('#vtxFreqErr').textContent = ''; vtxPost('freq = 0\n'); });
  $('#vtxReboot').addEventListener('click', () => power('reboot'));
  $('#vtxFile').addEventListener('change', async () => {
    const f = $('#vtxFile').files[0];
    $('#vtxFile').value = '';
    if (!f) return;
    if (f.size > 65536) { toast('That file is too big for a VTX table (64 KB at most).', 'bad'); return; }
    let text;
    try { text = await f.text(); } catch (e) { fail(e); return; }
    try {
      const r = await api('PUT', '/api/vtx/table', text);
      toast('VTX table loaded' + (r && r.description ? ': ' + r.description : '') + (r && isNum(r.bands) ? ' (' + r.bands + ' bands, ' + r.powers + ' power levels).' : '.'));
      V.fast = Date.now() + 4000;
    } catch (e) { toast('Not loaded: ' + e.message, 'bad', 9000); }
    V.sig = '';
    vtxLoop();
  });
  $('#vtxDownload').addEventListener('click', async () => {
    try { download('vtxtable.json', JSON.stringify(await api('GET', '/api/vtx/table'), null, 4) + '\n', 'application/json'); } catch (e) { fail(e); }
  });
  $('#vtxDefault').addEventListener('click', async () => {
    if (!await ask('Use the built-in VTX table?', 'The loaded table is deleted from the Pi. Download it first to keep a copy.', 'Use built-in')) return;
    try { await api('DELETE', '/api/vtx/table'); toast('The built-in VTX table is in use.'); } catch (e) { fail(e); }
    V.sig = '';
    vtxLoop();
  });
  document.addEventListener('settings', () => { if (page === 'video') { V.fast = Date.now() + 4000; vtxLoop(); } });

  // ---------------------------------------------------------------- pages and the router
  const TITLES = { status: 'Status', sticks: 'Sticks', video: 'Video', wifi: 'WiFi', sound: 'Sound', tracks: 'Tracks', game: 'Game', updates: 'Updates', system: 'System' };
  const noop = () => {};
  const PAGES = {
    status: { enter: () => { if ($('#shotImg').hidden) shot($('#shotImg'), $('#shotEmpty')); }, leave: noop },
    sticks: { enter: () => { gimbalLabels(); sticksFeed.start(); }, leave: () => sticksFeed.stop() },
    video: { enter: () => vtxLoop(), leave: () => clearTimeout(V.t) },
    wifi: { enter: () => { if (!$('#netList').children.length) lastScan(); }, leave: noop },
    sound: { enter: loadSongs, leave: noop },
    tracks: { enter: loadSlots, leave: noop },
    game: { enter: () => { loadTools(); if ($('#liveView').checked) liveLoop(); else remoteShot(); }, leave: () => clearTimeout(liveT) },
    updates: { enter: () => loadUpdate(), leave: () => clearTimeout(updT) },
    system: { enter: () => { loadSystem(); loadLog(); }, leave: () => clearTimeout(logT) },
  };
  let page = null;
  function route(user) {
    const id = location.hash.slice(1);
    const next = PAGES[id] ? id : 'status';
    if (next === page) return;
    if (page) PAGES[page].leave();
    page = next;
    for (const sec of $$('.page')) sec.hidden = sec.dataset.page !== page;
    for (const a of $$('#tabs a')) {
      if (a.dataset.page === page) { a.setAttribute('aria-current', 'page'); a.scrollIntoView({ block: 'nearest', inline: 'nearest' }); } else a.removeAttribute('aria-current');
    }
    document.title = TITLES[page] + ' · StickTime Pi';
    if (user) { window.scrollTo(0, 0); $('#h-' + page).focus({ preventScroll: true }); }
    PAGES[page].enter();
    statusLoop();
  }
  window.addEventListener('hashchange', () => route(true));

  // ---------------------------------------------------------------- theme
  // dark (the game's own menu colors) unless this browser keeps light, or auto (as the device)
  const THEMES = ['dark', 'light', 'auto'];
  const BAR = { dark: '#0e121c', light: '#e7eaee' };          // the browser's bar: --bg of each theme
  function theme(t) {
    if (!THEMES.includes(t)) t = 'dark';
    if (t === 'dark') delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = t;
    for (const m of $$('meta[name="theme-color"]')) m.content = BAR[t === 'auto' ? (m.media.includes('light') ? 'light' : 'dark') : t];
    const b = $('#themeBtn'), name = { auto: 'automatic', light: 'light', dark: 'dark' }[t];
    b.setAttribute('aria-label', 'Theme: ' + name + ' (change)');
    b.title = 'Theme: ' + name;
  }
  $('#themeBtn').addEventListener('click', () => {
    const cur = Math.max(0, THEMES.indexOf(store.get('theme'))), next = THEMES[(cur + 1) % THEMES.length];
    store.set('theme', next);
    theme(next);
    toast('Theme: ' + { auto: 'as the device', light: 'light', dark: 'dark, as in the game' }[next]);
  });
  theme(store.get('theme'));

  // ---------------------------------------------------------------- start
  route(false);
  loadSettings();
  loadUpdate(true);
  window.addEventListener('beforeunload', (e) => {
    if (forms.some((f) => !f.autosave && f.dirty().length)) { e.preventDefault(); e.returnValue = ''; }
  });
  // for the tests and for poking around in the console
  window.stp = { api, S, forms, get status() { return lastStatus; }, parseSettings, line, songName };
})();
