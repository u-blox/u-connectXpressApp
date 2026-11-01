/* u-connectXpressApp in the browser. The app runs as WebAssembly in this tab: it answers the page's /api calls
   and events here (no server), and reaches the NORA module over Web Serial (Chrome or Edge). */
(() => {
  'use strict';
  const realFetch = window.fetch.bind(window), RealEventSource = window.EventSource;
  let M = null, port = null, writer = null, reader = null, loop = null, reading = false;

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
    if (writer) writer.write(bytes).catch(e => console.warn('serial write', e));
  }

  async function readLoop(btn) {
    while (port && port.readable && reading) {
      reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          const p = M._malloc(value.length);
          M.HEAPU8.set(value, p);
          M._ucxapp_webserial_rx(p, value.length);
          M._free(p);
        }
      } catch (e) {
        if (reading) console.warn('serial read', e);
        break;
      } finally {
        reader.releaseLock();
        reader = null;
      }
    }
    if (reading) disconnect(btn);   /* unplugged or failed */
  }

  function exec(cmd) {
    return local('POST', '/api/exec?cmd=' + encodeURIComponent(cmd));
  }

  /* The app sees a dead port first (writes fail, reads end), then dev.disconnect closes its side. */
  async function disconnect(btn) {
    if (!port) return;
    const p = port, w = writer;
    reading = false;
    port = writer = null;
    M._ucxapp_webserial_ready(0);
    try {
      if (reader) await reader.cancel();
      await loop;
      if (w) await w.close();
      await p.close();
    } catch (e) {
      console.warn('serial close', e);
    }
    await exec('dev.disconnect');
    btn.textContent = 'Connect module';
  }

  async function connect(btn) {
    if (port) return disconnect(btn);
    if (!navigator.serial) {
      alert('This browser has no Web Serial. Use Chrome or Edge on a desktop.');
      return;
    }
    await ready;
    try {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200, bufferSize: 65536 });
    } catch (e) {
      port = null;
      if (e.name !== 'NotFoundError') alert('Could not open the port: ' + e.message);
      return;
    }
    writer = port.writable.getWriter();
    M._ucxapp_webserial_ready(1);
    reading = true;
    loop = readLoop(btn);
    btn.textContent = 'Disconnect module';
    const cmd = document.getElementById('cmd'), run = document.getElementById('run');
    if (cmd && run) { cmd.value = 'dev.connect webserial'; run.click(); } else exec('dev.connect webserial');
  }

  function addButton() {
    const files = document.getElementById('files');
    if (!files) return;
    const btn = document.createElement('button');
    btn.id = 'wsc';
    btn.textContent = 'Connect module';
    btn.title = 'Pick the NORA EVK serial port (Web Serial, 115200 baud)';
    btn.onclick = () => connect(btn);
    files.parentNode.insertBefore(btn, files);
    if (navigator.serial) {
      navigator.serial.addEventListener('disconnect', e => { if (e.target === port) disconnect(btn); });
    }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', addButton);
  else addButton();
})();
