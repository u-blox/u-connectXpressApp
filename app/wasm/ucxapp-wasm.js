/* u-connectXpressApp in the browser. The app runs as WebAssembly in this tab: it answers the page's /api calls
   and events here (no server), and reaches the NORA module over Web Serial (Chrome or Edge), or a virtual
   NORA-W36 that answers from vnora.json (the vucx test profile) when there is no hardware. */
(() => {
  'use strict';
  const realFetch = window.fetch.bind(window), RealEventSource = window.EventSource;
  let M = null, port = null, writer = null, reader = null, loop = null, reading = false, virt = null, opening = false;
  const LAST_PORT = 'ucxapp.lastPort';

  /* Without cross-origin isolation (first visit, before the service worker reloads the page) threads cannot start. */
  const ready = !self.crossOriginIsolated ? new Promise(() => {}) : createUcxapp({
    print: t => console.log(t),
    printErr: t => console.warn(t),
  }).then(m => { M = m; m.ucxSerialWrite = serialWrite; return m; });

  /* ---- the page's /api calls go to the app ---- */
  async function local(method, url, body) {
    await ready;
    const u = new URL(url, location.href);
    const data = body == null ? new Uint8Array(0) : new Uint8Array(await new Response(body).arrayBuffer());
    const p = M._malloc(data.length + 1);
    M.HEAPU8.set(data, p);
    const status = M.ccall('ucxapp_web_local_request', 'number', ['string', 'string', 'string', 'number', 'number'],
      [method, u.pathname, u.search.slice(1), p, data.length]);
    M._free(p);
    const ptr = M._ucxapp_web_local_body(), len = M._ucxapp_web_local_length();
    const reply = M.HEAPU8.slice(ptr, ptr + len);   /* a copy: the heap is shared memory */
    return new Response(status === 204 ? null : reply,
      { status, headers: { 'Content-Type': M.UTF8ToString(M._ucxapp_web_local_type()) || 'text/plain' } });
  }

  window.fetch = (input, init) => {
    const url = typeof input === 'string' ? input : input.url;
    if (!new URL(url, location.href).pathname.startsWith('/api/')) return realFetch(input, init);
    return local(((init && init.method) || 'GET').toUpperCase(), url, init && init.body);
  };

  /* ---- the event stream is polled from the app ---- */
  class LocalEvents {
    constructor() {
      this.l = {};
      ready.then(() => {
        if (this.onopen) this.onopen();
        this.t = setInterval(() => this.poll(), 25);
      });
    }
    addEventListener(name, fn) { (this.l[name] = this.l[name] || []).push(fn); }
    close() { clearInterval(this.t); }
    poll() {
      for (const line of M.UTF8ToString(M._ucxapp_web_local_poll()).split('\n')) {
        const i = line.indexOf('\t');
        if (i > 0) (this.l[line.slice(0, i)] || []).forEach(fn => fn({ data: line.slice(i + 1) }));
      }
    }
  }
  window.EventSource = function (url, opts) {
    return new URL(url, location.href).pathname === '/events' ? new LocalEvents() : new RealEventSource(url, opts);
  };

  /* ---- Web Serial: the module's UART ---- */
  function serialWrite(ptr, len) {
    const bytes = M.HEAPU8.slice(ptr, ptr + len);
    M._free(ptr);
    if (virt) virtWrite(bytes);
    else if (writer) writer.write(bytes).catch(e => console.warn('serial write', e));
  }

  function rx(bytes) {
    const p = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, p);
    M._ucxapp_webserial_rx(p, bytes.length);
    M._free(p);
  }

  /* ---- the virtual module: exact command or * ? pattern [state] -> response lines, then URCs; lists of lists
     answer in turn, \x01 starts binary data (like tests/harness/vucx) */
  function lookup(v, table, cmd) {
    for (const exact of [true, false]) {
      for (const [key, lines] of Object.entries(table || {})) {
        const at = key.indexOf(' ['), pattern = at < 0 ? key : key.slice(0, at);
        const cond = at < 0 ? [] : key.slice(at + 2, -1).split(' ').filter(Boolean);
        const re = '^' + pattern.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$';
        if (!(exact ? pattern === cmd : new RegExp(re).test(cmd))) continue;
        if (!cond.every(c => (v.state[c.split('=')[0]] || '') === c.slice(c.indexOf('=') + 1))) continue;
        if (lines.length && lines.every(Array.isArray)) {
          const n = v.turn[key] || 0;
          v.turn[key] = n + 1;
          return lines[n % lines.length];
        }
        return lines;
      }
    }
    return null;
  }

  function virtSend(v, lines) {
    if (virt !== v) return;
    const enc = new TextEncoder(), out = [];
    for (const line of lines) {
      const i = line.indexOf('\x01'), text = i < 0 ? line : line.slice(0, i);
      out.push(...enc.encode('\r\n' + text));
      if (i >= 0) {
        const data = enc.encode(line.slice(i + 1));
        out.push(1, data.length >> 8, data.length & 0xff, ...data);
      }
    }
    out.push(13, 10);
    rx(new Uint8Array(out));
  }

  function virtWrite(bytes) {
    const v = virt, b = new Uint8Array(v.buf.length + bytes.length);
    b.set(v.buf);
    b.set(bytes, v.buf.length);
    v.buf = b;
    for (;;) {
      const cr = v.buf.indexOf(13), soh = v.buf.indexOf(1);
      let end, next;
      if (soh >= 0 && (cr < 0 || soh < cr)) {
        if (v.buf.length < soh + 3) break;
        next = soh + 3 + (v.buf[soh + 1] << 8 | v.buf[soh + 2]);   /* binary data follows the command */
        if (v.buf.length < next) break;
        end = soh;
      } else if (cr >= 0) {
        end = cr;
        next = cr + 1;
      } else {
        break;
      }
      const cmd = new TextDecoder().decode(v.buf.slice(0, end)).trim();
      v.buf = v.buf.slice(next);
      if (!cmd) continue;
      Object.assign(v.state, lookup(v, v.state_set, cmd) || {});
      const reply = lookup(v, v.commands, cmd) || v.default, urcs = lookup(v, v.urcs, cmd);
      setTimeout(() => virtSend(v, reply), 2);
      if (urcs) setTimeout(() => virtSend(v, urcs), 50);
    }
  }

  async function readLoop() {
    while (port && port.readable && reading) {
      reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          rx(value);
        }
      } catch (e) {
        if (reading) console.warn('serial read', e);
        break;
      } finally {
        reader.releaseLock();
        reader = null;
      }
    }
    if (reading) disconnect();   /* unplugged or failed */
  }

  function exec(cmd) {
    return local('POST', '/api/exec?cmd=' + encodeURIComponent(cmd));
  }

  function connected(yes) {
    document.getElementById('wsc').textContent = yes ? 'Disconnect module' : 'Connect module';
    document.getElementById('wsv').hidden = yes;
  }

  function devConnect() {
    M._ucxapp_webserial_ready(1);
    connected(true);
    const cmd = document.getElementById('cmd'), run = document.getElementById('run');
    if (cmd && run) { cmd.value = 'dev.connect webserial'; run.click(); } else exec('dev.connect webserial');
  }

  /* The app sees a dead port first (writes fail, reads end), then dev.disconnect closes its side. */
  async function disconnect(forget) {
    if (!port && !virt) return;
    if (forget === true && port) localStorage.removeItem(LAST_PORT);
    const p = port, w = writer;
    reading = false;
    port = writer = virt = null;
    M._ucxapp_webserial_ready(0);
    try {
      if (reader) await reader.cancel();
      await loop;
      if (w) await w.close();
      if (p) await p.close();
    } catch (e) {
      console.warn('serial close', e);
    }
    await exec('dev.disconnect');
    connected(false);
  }

  async function connectVirtual() {
    await ready;
    try {
      const r = await realFetch('vnora.json');
      const t = await r.json();
      virt = Object.assign(t, { state_set: t.state, state: {}, turn: {}, buf: new Uint8Array(0) });
    } catch (e) {
      alert('Could not load the virtual module: ' + e.message);
      return;
    }
    devConnect();
  }

  async function openPort(p) {
    await p.open({ baudRate: 115200, bufferSize: 65536 });
    port = p;
    const ports = await navigator.serial.getPorts(), info = p.getInfo();
    localStorage.setItem(LAST_PORT, JSON.stringify({ index: ports.indexOf(p), vid: info.usbVendorId,
      pid: info.usbProductId }));
    writer = port.writable.getWriter();
    reading = true;
    loop = readLoop();
    devConnect();
  }

  /* A port this site was granted before: the remembered index if it is still the same USB device, else the
     first one with the same vendor and product id. */
  async function lastPort() {
    let last;
    try {
      last = JSON.parse(localStorage.getItem(LAST_PORT));
    } catch (e) {
      last = null;
    }
    if (!last || !navigator.serial) return null;
    const ports = await navigator.serial.getPorts();
    const same = p => { const i = p.getInfo(); return i.usbVendorId === last.vid && i.usbProductId === last.pid; };
    return ports[last.index] && same(ports[last.index]) ? ports[last.index] : ports.find(same) || null;
  }

  async function reopen() {
    if (port || virt || opening) return;
    opening = true;
    try {
      await ready;
      const p = await lastPort();
      /* After a reload the old page may still hold the port for a moment: retry before giving up. */
      for (let i = 0; p && !port && !virt; i++) {
        try {
          await openPort(p);
        } catch (e) {
          port = null;
          if (i >= 20) throw e;
          await new Promise(r => setTimeout(r, 150));
        }
      }
    } catch (e) {
      port = null;
      console.warn('reopen last port', e);   /* busy in another tab or app: the button still works */
    } finally {
      opening = false;
    }
  }

  async function connect() {
    if (port || virt) return disconnect(true);
    if (!navigator.serial) {
      alert(noSerialReason());
      return;
    }
    await ready;
    let p;
    try {
      p = await navigator.serial.requestPort();
      await openPort(p);
    } catch (e) {
      port = null;
      if (e.name !== 'NotFoundError') alert('Could not open the port: ' + e.message);
    }
  }

  function addButton(id, text, title, onclick) {
    const files = document.getElementById('files');
    const btn = document.createElement('button');
    btn.id = id;
    btn.textContent = text;
    btn.title = title;
    btn.onclick = onclick;
    files.parentNode.insertBefore(btn, files);
  }

  /* Web Serial is Chromium only (Chrome, Edge, Opera) and needs https or localhost. */
  function noSerialReason() {
    if (navigator.serial) return '';
    if (!window.isSecureContext) return 'Web Serial needs https or localhost.';
    return 'This browser has no Web Serial (Firefox and Safari do not support it). Use Chrome or Edge on a ' +
      'desktop to connect a module, or try the virtual module.';
  }

  function addButtons() {
    const files = document.getElementById('files');
    if (!files) return;
    const reason = noSerialReason();
    addButton('wsc', 'Connect module', reason || 'Pick the NORA EVK serial port (Web Serial, 115200 baud)', connect);
    addButton('wsv', 'Virtual module', 'No hardware: a simulated NORA-W36 with Wi-Fi, sockets and Bluetooth',
      connectVirtual);
    if (reason) {
      const note = document.createElement('span');
      note.id = 'wsnote';
      note.textContent = reason;
      note.style.cssText = 'color:#b45309;font-size:12px;margin:0 6px';
      files.parentNode.insertBefore(note, files);
    }
    if (navigator.serial) {
      navigator.serial.addEventListener('disconnect', e => { if (e.target === port) disconnect(); });
      navigator.serial.addEventListener('connect', reopen);   /* the remembered EVK plugged back in */
      window.addEventListener('pagehide', () => {
        reading = false;
        try { if (reader) reader.cancel(); if (writer) writer.releaseLock(); if (port) port.close(); } catch (e) { /* unloading */ }
      });
      reopen();
    }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', addButtons);
  else addButtons();
})();
