/* u-connectXpressApp in the browser. The app runs as WebAssembly in this tab: it answers the page's /api calls
   and events here (no server), and reaches the NORA module over Web Serial (Chrome or Edge), or a virtual
   NORA-W36 (vnora.json) or ALMA-W1 with Thread (valma-w1.json), from the vucx test profiles, when there is
   no hardware. */
(() => {
  'use strict';
  const realFetch = window.fetch.bind(window), RealEventSource = window.EventSource;
  let M = null, port = null, writer = null, reader = null, loop = null, reading = false, virt = null, opening = false, closing = null;
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

  /* ---- the event stream is polled from the app; a worker keeps the time, as a hidden tab runs the page's
     own timers only once a second (once a minute after five) ---- */
  class LocalEvents {
    constructor() {
      this.l = {};
      ready.then(() => {
        if (this.onopen) this.onopen();
        try {
          this.w = new Worker(URL.createObjectURL(new Blob(['setInterval(() => postMessage(0), 25)'])));
          this.w.onmessage = () => this.poll();
        } catch (e) {
          this.t = setInterval(() => this.poll(), 25);
        }
      });
    }
    addEventListener(name, fn) { (this.l[name] = this.l[name] || []).push(fn); }
    close() {
      if (this.w) this.w.terminate();
      clearInterval(this.t);
    }
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
      if (Array.isArray(line)) {   /* [text, bytes]: binary data from a socket read */
        out.push(...enc.encode('\r\n' + line[0]), 1, line[1].length >> 8, line[1].length & 0xff, ...line[1]);
        continue;
      }
      const i = line.indexOf('\x01'), text = i < 0 ? line : line.slice(0, i);
      out.push(...enc.encode('\r\n' + text));
      if (i >= 0) {
        const data = Array.from(line.slice(i + 1), c => c.charCodeAt(0) & 0xff);   /* latin-1, like the harness */
        out.push(1, data.length >> 8, data.length & 0xff, ...data);
      }
    }
    out.push(13, 10);
    rx(new Uint8Array(out));
  }

  /* uploaded certificates by name; the fingerprint is the SHA-256 of the DER, as the module and the harness give */
  function certHandle(v, cmd, data) {
    let m = /^AT\+USECUB=\d+,"([^"]+)"/.exec(cmd);
    if (m && data.length) {
      (v.certs = v.certs || {})[m[1]] = data;
      return null;
    }
    m = /^AT\+USECD="([^"]+)",0$/.exec(cmd);
    const cert = m && v.certs && v.certs[m[1]];
    if (!cert || !crypto.subtle) return null;
    const pem = /-----BEGIN [^-]+-----([\s\S]*?)-----END/.exec(String.fromCharCode(...cert));
    const der = pem ? Uint8Array.from(atob(pem[1].replace(/\s+/g, '')), c => c.charCodeAt(0)) : cert;
    return crypto.subtle.digest('SHA-256', der).then(h =>
      ['+USECD:0,' + Array.from(new Uint8Array(h), x => x.toString(16).padStart(2, '0')).join('').toUpperCase(), 'OK']);
  }

  /* our own GATT server (gatt_server in the profile), as GattServer in tests/harness/vucx: handles count up from 40
     since the last reboot (a service on the next multiple of 20, a characteristic declaration/value/CCCD, a descriptor
     one); URC lines name the first characteristic matching {h:uuid} or {c:uuid} (its CCCD) and are left out when there
     is none; those of AT+UBTGSA only see the services being activated */
  function gattHandle(v, cmd) {
    const g = v.gatt = v.gatt || { next: 40, chars: [], active: 0 };
    if (cmd.startsWith('AT+UBTGS=')) {
      const service = Math.ceil(g.next / 20) * 20;
      g.next = service + 1;
      return ['+UBTGS:' + service, 'OK'];
    }
    const m = /^AT\+UBTG(C|HCC)=([0-9A-Fa-f]*)/.exec(cmd);
    if (m) {
      const value = g.next + 1, cccd = g.next + 2;
      g.next += 3;
      g.chars.push([m[2].toUpperCase(), value, cccd]);
      return ['+UBTG' + m[1] + ':' + value + ',' + cccd, 'OK'];
    }
    if (!cmd.startsWith('AT+UBTGD=')) return null;
    g.next++;
    return ['+UBTGD:' + (g.next - 1), 'OK'];
  }

  function gattFill(v, lines, cmd) {
    if (!v.gatt_server || !lines) return lines;
    const g = v.gatt || { chars: [], active: 0 };
    const chars = cmd === 'AT+UBTGSA' ? g.chars.slice(g.active) : g.chars;
    const out = [];
    for (const line of lines) {
      let missing = false;
      const text = typeof line !== 'string' ? line : line.replace(/\{([hc]):([0-9A-Fa-f]+)\}/g, (all, kind, prefix) => {
        const hit = chars.find(c => c[0] && c[0].startsWith(prefix.toUpperCase()));
        if (!hit) missing = true;
        return hit ? String(hit[kind === 'h' ? 1 : 2]) : all;
      });
      if (!missing) out.push(text);
    }
    return out;
  }
  function virtWrite(bytes) {
    const v = virt, b = new Uint8Array(v.buf.length + bytes.length);
    b.set(v.buf);
    b.set(bytes, v.buf.length);
    v.buf = b;
    for (;;) {
      const cr = v.buf.indexOf(13), soh = v.buf.indexOf(1);
      let end, next, data = new Uint8Array(0);
      if (soh >= 0 && (cr < 0 || soh < cr)) {
        if (v.buf.length < soh + 3) break;
        next = soh + 3 + (v.buf[soh + 1] << 8 | v.buf[soh + 2]);   /* binary data follows the command */
        if (v.buf.length < next) break;
        end = soh;
        data = v.buf.slice(soh + 3, next);
      } else if (cr >= 0) {
        end = cr;
        next = cr + 1;
      } else {
        break;
      }
      const cmd = new TextDecoder().decode(v.buf.slice(0, end)).trim();
      v.buf = v.buf.slice(next);
      if (!cmd) continue;
      const cert = certHandle(v, cmd, data);
      if (cert) {
        cert.then(lines => soon(() => virtSend(v, lines)));
        continue;
      }
      const net = cmd.startsWith('AT') ? ftBle(v, cmd) || (v.net ? netHandle(v, cmd, data) : null) : null;
      Object.assign(v.state, lookup(v, v.state_set, cmd) || {});
      const reply = net ? net[0] : lookup(v, v.commands, cmd) || (v.gatt_server && gattHandle(v, cmd)) || v.default;
      if (cmd === 'AT+CPWROFF') v.gatt = null;
      const urcs = gattFill(v, net ? (net[1].length ? net[1] : null) : lookup(v, v.urcs, cmd), cmd);
      if (cmd === 'AT+UBTGSA' && v.gatt) v.gatt.active = v.gatt.chars.length;
      /* not setTimeout: a hidden tab runs those once a second (once a minute after five), and every AT
         command would wait that long */
      soon(() => {
        virtSend(v, reply);
        if (urcs) soon(() => virtSend(v, urcs));
      });
    }
  }

  const soonQueue = [], soonChannel = new MessageChannel();
  soonChannel.port1.onmessage = () => { const f = soonQueue.shift(); if (f) f(); };
  function soon(f) {
    soonQueue.push(f);
    soonChannel.port2.postMessage(0);
  }

  /* ---- the network behind the virtual module (AT+USO*), as Network in tests/harness/vucx: an echo, a CoAP
     server or an NTRIP caster (its test stream, then an RTCM 1005 once a second) at the far end ---- */
  const unhex = h => new Uint8Array((String(h || '').match(/../g) || []).map(x => parseInt(x, 16)));

  function netNew(v, proto, port) {
    let n = 0;
    while (v.socks[n]) n++;
    v.socks[n] = { proto, port: port || 0, rx: [], state: 0, status: 1 };
    return n;
  }

  function netAvail(v, n) {
    const s = v.socks[n], len = s ? s.rx.reduce((a, x) => a + x[2].length, 0) : 0;
    return len ? [`+UESODA:${n},${len}`] : [];
  }

  function netServe(v, n, s, data, ip = '192.168.1.10', port = s.port) {
    const kind = v.net.servers[String(s.port)] || 'echo';
    if (kind === 'ntrip' && s.state === 0) {
      const head = new TextEncoder().encode('ICY 200 OK\r\n\r\n'), body = unhex(v.net.ntrip), b = new Uint8Array(head.length + body.length);
      b.set(head);
      b.set(body, head.length);
      s.state = 1;
      s.rx.push([ip, port, b]);
      s.timer = setInterval(() => {
        if (virt !== v || v.socks[n] !== s) { clearInterval(s.timer); return; }
        s.rx.push([ip, port, unhex(v.net.rtcm)]);
        virtSend(v, netAvail(v, n));
      }, 1000);
    } else if (kind === 'coap' && data.length >= 4) {
      const tkl = data[0] & 0x0f, type = (data[0] >> 4) & 3;
      const head = [0x40 | ((type === 0 ? 2 : 1) << 4) | tkl, 0x45, ...data.slice(2, 4 + tkl)];
      s.rx.push([ip, port, new Uint8Array([...head, 0xff, ...new TextEncoder().encode('21.5')])]);
    } else if (kind === 'ntp' && data.length >= 48) {
      const b = new Uint8Array(48), sec = (Math.floor(Date.now() / 1000) + 2208988800) >>> 0;
      b[0] = 0x24;
      b[1] = 2;
      new DataView(b.buffer).setUint32(40, sec);
      s.rx.push([ip, port, b]);
    } else if (kind === 'tftp' && data.length >= 4) {
      const op = (data[0] << 8) | data[1];
      if (op === 1) {   /* RRQ: one short DATA block is the whole file */
        const end = data.indexOf(0, 2), name = String.fromCharCode(...data.slice(2, end < 0 ? data.length : end));
        s.rx.push([ip, port, new Uint8Array([0, 3, 0, 1, ...new TextEncoder().encode(`vucx demo file ${name}\n`)])]);
      } else if (op === 2 || op === 3) {   /* WRQ, DATA: acknowledge */
        s.rx.push([ip, port, new Uint8Array([0, 4, op === 3 ? data[2] : 0, op === 3 ? data[3] : 0])]);
      }
    } else if (kind === 'http') {
      const head = String.fromCharCode(...data.slice(0, 5)) === 'HEAD ';
      s.rx.push([ip, port, new TextEncoder().encode(`HTTP/1.1 200 OK\r\nDate: ${new Date().toUTCString()}\r\n` +
        'Content-Type: text/html\r\nContent-Length: 17\r\nConnection: close\r\n\r\n' + (head ? '' : '<html>vucx</html>'))]);
    } else if (kind === 'ft') {   /* [len u16][channel][packet] frames both ways; the replies go as one read */
      let acc = s.acc = concat(s.acc || new Uint8Array(0), data);
      const out = [];
      while (acc.length >= 3 && acc.length >= 3 + (acc[0] | acc[1] << 8)) {
        const n = 3 + (acc[0] | acc[1] << 8);
        for (const [isData, pkt] of ftPacket(v, s, acc[2] === 1, acc.slice(3, n))) {
          out.push(pkt.length & 0xff, pkt.length >> 8, isData ? 1 : 0, ...pkt);
        }
        acc = s.acc = acc.slice(n);
      }
      if (out.length) s.rx.push([ip, port, new Uint8Array(out)]);
    } else if (kind === 'echo') {
      s.rx.push([ip, port, data]);
    }
  }

  /* the ft file server (ft.on on the other module), as FtServer in tests/harness/vucx: one client packet in, the
     replies out as [data, packet]: a whole window of blocks, the next on each ACK, an ACK every 8 upload blocks */
  const concat = (a, b) => { const c = new Uint8Array(a.length + b.length); c.set(a); c.set(b, a.length); return c; };
  const le16 = x => [x & 0xff, (x >> 8) & 0xff], le32 = x => [x & 0xff, (x >> 8) & 0xff, (x >> 16) & 0xff, (x >>> 24) & 0xff];
  let crcTable = null;
  function crc32(b) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        crcTable[i] = c >>> 0;
      }
    }
    let c = 0xffffffff;
    for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  function ftWindow(st, base) {
    const out = [], blocks = Math.floor(st.body.length / st.blk) + 1;
    for (let b = base; b < Math.min(base + 8, blocks); b++) {
      out.push([true, new Uint8Array([...le16(b), ...st.body.slice(b * st.blk, (b + 1) * st.blk)])]);
    }
    return out;
  }

  function ftPacket(v, link, isData, pkt) {
    const ft = v.ft = v.ft || {
      files: { 'notes.txt': new TextEncoder().encode('notes.txt: a demo file on the virtual ft server\n'),
               'big.bin': Uint8Array.from({ length: 20000 }, (_, i) => (i * 7 + (i >> 8)) & 0xff) },
      links: new Map() };
    if (!ft.links.has(link)) ft.links.set(link, {});
    const st = ft.links.get(link), u16 = a => pkt[a] | pkt[a + 1] << 8;
    const u32 = a => (pkt[a] | pkt[a + 1] << 8 | pkt[a + 2] << 16 | pkt[a + 3] << 24) >>> 0;
    const text = a => String.fromCharCode(...pkt.slice(a)), clear = () => Object.keys(st).forEach(k => delete st[k]);
    const ctrl = bytes => [false, new Uint8Array(bytes)];
    if (isData) {
      if (st.mode !== 'put' || pkt.length < 2) return [];
      if (u16(0) !== st.want) return [ctrl([0x85, ...le16(st.want)])];
      st.buf = concat(st.buf, pkt.slice(2));
      st.want++;
      st.since++;
      if (pkt.length - 2 < st.blk) {   /* the short block ends the file */
        const ok = st.buf.length === st.size && crc32(st.buf) === st.crc;
        if (ok) ft.files[st.name] = st.buf;
        const out = [ctrl([0x85, ...le16(st.want)]), ctrl([0x87, ok ? 0 : 6])];
        clear();
        return out;
      }
      if (st.since >= 8) {
        st.since = 0;
        return [ctrl([0x85, ...le16(st.want)])];
      }
      return [];
    }
    if (!pkt.length) return [];
    const op = pkt[0];
    if (op === 0x01 || op === 0x07) {   /* GET, GET from an offset */
      const hdr = op === 0x07 ? 7 : 3, f = ft.files[text(hdr)], off = op === 0x07 ? u32(3) : 0;
      if (!f || off > f.length || u16(1) < 16) return [ctrl([0x81, f ? 4 : 1])];
      clear();
      Object.assign(st, { mode: 'get', body: f.slice(off), blk: u16(1) });
      return [ctrl([0x81, 0, ...le32(f.length), ...le32(crc32(f))])].concat(ftWindow(st, 0));
    }
    if (op === 0x05 && st.mode === 'get') {
      if (u16(1) > Math.floor(st.body.length / st.blk)) { clear(); return []; }
      return ftWindow(st, u16(1));
    }
    if (op === 0x02 || op === 0x08) {   /* PUT, PUT resume (no .part is kept: it starts over) */
      if (pkt.length < 12 || u16(1) < 16) return [ctrl([0x80 | op, 4])];
      clear();
      Object.assign(st, { mode: 'put', blk: u16(1), size: u32(3), crc: u32(7), name: text(11), buf: new Uint8Array(0),
                          want: 0, since: 0 });
      return [ctrl(op === 0x08 ? [0x88, 0, 0, 0, 0, 0] : [0x82, 0])];
    }
    if (op === 0x03) {
      return Object.entries(ft.files).map(([n, f]) => ctrl([0x83, 0, ...le32(f.length), ...new TextEncoder().encode(n)]))
        .concat([ctrl([0x83, 0xff])]);
    }
    if (op === 0x04) {
      const name = text(1), had = name in ft.files;
      delete ft.files[name];
      return [ctrl([0x84, had ? 0 : 1])];
    }
    if (op === 0x06) clear();
    return [];
  }

  /* a write to the BLE peer's ft CTRL/DATA characteristic: the replies come back as notifications */
  function ftBle(v, cmd) {
    const m = /^AT\+UBTGWNR=(\d+),(\d+),([0-9A-Fa-f]*)$/.exec(cmd);
    if (!m || !v.ft_peer || !v.ft_peer.includes(+m[2])) return null;
    const [ctrlH, dataH] = v.ft_peer, hex = b => Array.from(b, x => x.toString(16).padStart(2, '0')).join('').toUpperCase();
    const out = ftPacket(v, 'ble', +m[2] === dataH, unhex(m[3]));
    return [['OK'], out.map(([d, p]) => `+UEBTGCN:${m[1]},${d ? dataH : ctrlH},${hex(p)}`)];
  }

  /* [response lines, URC lines] for a socket command, null for any other command */
  function netHandle(v, cmd, data) {
    let m;
    if ((m = /^AT\+USOCR=(\d+)(?:,\d+)?$/.exec(cmd))) return [[`+USOCR:${netNew(v, +m[1])}`, 'OK'], []];
    if ((m = /^AT\+USOC=(\d+),"?([^",]*)"?,(\d+)$/.exec(cmd)) && v.socks[m[1]]) {
      Object.assign(v.socks[m[1]], { port: +m[3], status: 2 });
      return [['OK'], [`+UESOC:${m[1]}`]];
    }
    if ((m = /^AT\+USOWB=(\d+),?$/.exec(cmd)) && v.socks[m[1]]) {
      netServe(v, +m[1], v.socks[m[1]], data);
      return [[`+USOWB:${m[1]},${data.length}`, 'OK'], netAvail(v, m[1])];
    }
    if ((m = /^AT\+USOSTB=(\d+),([\d.]+),(\d+),?$/.exec(cmd)) && v.socks[m[1]]) {
      netServe(v, -1, Object.assign({}, v.socks[m[1]], { port: +m[3] }), data, m[2], +m[3]);
      return [[`+USOSTB:${m[1]},${data.length}`, 'OK'], netAvail(v, m[1])];
    }
    if ((m = /^AT\+USOR(F?)B=(\d+),(\d+)$/.exec(cmd)) && v.socks[m[2]]) {
      const rx = v.socks[m[2]].rx, size = +m[3];
      let [ip, port, chunk] = rx.length ? rx.shift() : ['0.0.0.0', 0, new Uint8Array(0)];
      if (chunk.length > size && !m[1]) rx.unshift([ip, port, chunk.slice(size)]);
      chunk = chunk.slice(0, size);
      return [[[m[1] ? `+USORFB:${m[2]},${ip},${port}` : `+USORB:${m[2]}`, chunk], 'OK'], []];
    }
    if ((m = /^AT\+USOCL=(\d+)$/.exec(cmd))) {
      const s = v.socks[m[1]];
      if (s) { clearInterval(s.timer); delete v.socks[m[1]]; }
      return [[s ? 'OK' : 'ERROR'], []];
    }
    if ((m = /^AT\+USO([BL])=(\d+)(?:,(\d+))?$/.exec(cmd)) && v.socks[m[2]]) {
      const port = +(m[3] || 0);
      Object.assign(v.socks[m[2]], { port, status: m[1] === 'L' ? 3 : 1 });
      if (m[1] === 'L' && v.net.accept.includes(port)) return [['OK'], [`+UESOIC:${netNew(v, 6, port)},192.168.1.10,${m[2]}`]];
      if (m[1] === 'B' && v.socks[m[2]].proto === 17 && v.net.accept.includes(port)) {
        v.socks[m[2]].rx.push(['192.168.1.10', 50000, new TextEncoder().encode('hello from the vucx demo client')]);
        return [['OK'], netAvail(v, m[2])];
      }
      return [['OK'], []];
    }
    if (cmd === 'AT+USOST?') return [Object.entries(v.socks).map(([n, s]) => `+USOST:${n},${s.proto},${s.status}`).concat(['OK']), []];
    /* SPS: the BLE peer echoes (perf.echo sps on module B) */
    const sps = v.sps = v.sps || {};
    if ((m = /^AT\+USPSC=(\d+)(?:,\d+)?$/.exec(cmd))) {
      sps[m[1]] = new Uint8Array(0);
      return [['OK'], [`+UESPSC:${m[1]}`]];
    }
    if ((m = /^AT\+USPSWB=(\d+),?$/.exec(cmd)) && sps[m[1]]) {
      const b = new Uint8Array(sps[m[1]].length + data.length);
      b.set(sps[m[1]]);
      b.set(data, sps[m[1]].length);
      sps[m[1]] = b;
      return [[`+USPSWB:${m[1]},${data.length}`, 'OK'], [`+UESPSDA:${m[1]},${b.length}`]];
    }
    if ((m = /^AT\+USPSRB=(\d+),(\d+)$/.exec(cmd)) && sps[m[1]]) {
      const chunk = sps[m[1]].slice(0, +m[2]);
      sps[m[1]] = sps[m[1]].slice(+m[2]);
      return [[[`+USPSRB:${m[1]}`, chunk], 'OK'], []];
    }
    return null;
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
    document.getElementById('wsvt').hidden = yes;
  }

  async function devConnect() {
    const mine = port || virt;
    connected(true);
    await closing;
    if ((port || virt) !== mine) return;   /* closed again while the previous close finished */
    M._ucxapp_webserial_ready(1);
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
    connected(false);
    /* a command still running on the dead port (dev.summary times out) answers 409: retry until it is
       queued, then wait until it has run; the next connect waits for this so it finds the module closed */
    const wait = () => new Promise(r => setTimeout(r, 100));
    closing = (async () => {
      try {
        if (reader) await reader.cancel();
        await loop;
        if (w) await w.close();
        if (p) await p.close();
      } catch (e) {
        console.warn('serial close', e);
      }
      for (let i = 0; (await exec('dev.disconnect')).status === 409 && i < 600; i++) await wait();
      for (let i = 0; M._ucxapp_web_local_busy() && i < 600; i++) await wait();
    })();
    await closing;
  }

  async function connectVirtual(file) {
    await ready;
    try {
      const r = await realFetch(file);
      const t = await r.json();
      virt = Object.assign(t, { state_set: t.state, state: {}, turn: {}, buf: new Uint8Array(0), net: t.sockets, socks: {} });
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
      () => connectVirtual('vnora.json'));
    addButton('wsvt', 'Virtual Thread module', 'No hardware: a simulated ALMA-W1 with Thread, Wi-Fi and Bluetooth',
      () => connectVirtual('valma-w1.json'));
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
