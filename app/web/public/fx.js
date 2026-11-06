/* Public site only (GitHub Pages, ucxapp --wasm): motion on top of app/web/index.html. It only watches the page;
   every command still goes through the page script and the app. */
(() => {
  'use strict';
  /* off by default (plain, like the app's own page); the FX button or a ?fx=1 link turns it on */
  const calm = localStorage.getItem('fxoff') !== '0';
  const SVG = 'http://www.w3.org/2000/svg';
  const COLORS = ['#29b6f6', '#a07cff', '#66d17a', '#ffd54a', '#ff3b6b', '#4f8cff'];
  const make = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };
  {
    const b = make('button', calm ? '' : 'on'), files = document.getElementById('files');
    b.id = 'fx-tg'; b.textContent = 'FX'; b.title = calm ? 'Turn animations on' : 'Turn animations off';
    b.onclick = () => { localStorage.setItem('fxoff', calm ? '0' : '1'); location.reload(); };
    if (files) files.before(b);
    if (calm) document.body.classList.add('fx-off');
  }

  /* ---- splash: logo with radio waves until the app is up ---- */
  if (!calm && !sessionStorage.getItem('fxs')) {
    sessionStorage.setItem('fxs', '1');
    const s = make('div'), w = make('div', 'w'), p = make('p'), small = make('small');
    s.id = 'fx-splash';
    for (let i = 0; i < 3; i++) w.appendChild(make('span', 'r'));
    const img = make('img'); img.src = 'icon.svg'; img.alt = ''; w.appendChild(img);
    [...'u-connectXpress'].forEach((ch, i) => { const c = make('span'); c.textContent = ch; c.style.animationDelay = (300 + i * 45) + 'ms'; p.appendChild(c); });
    small.textContent = 'in your browser, over Web Serial';
    const box = make('div'); box.append(w, p, small); s.appendChild(box);
    document.body.appendChild(s);
    const t0 = performance.now();
    const gone = () => setTimeout(() => { s.classList.add('gone'); setTimeout(() => s.remove(), 800); },
                                  Math.max(0, 1500 - (performance.now() - t0)));
    if (document.readyState === 'complete') gone(); else addEventListener('load', gone);
    s.onclick = () => s.classList.add('gone');
  }

  /* ---- idle: the endless motion stops after a while without input (no CPU while you read) and comes back with
     the next move or key ---- */
  const IDLE_MS = 15000;
  let wake = () => {};
  if (!calm) {
    let last = performance.now();
    const idle = on => {
      document.body.classList.toggle('fx-idle', on);
      const dg = document.getElementById('dg');
      if (dg && dg.pauseAnimations) { if (on) dg.pauseAnimations(); else dg.unpauseAnimations(); }
      if (!on) wake();
    };
    const input = () => { last = performance.now(); if (document.body.classList.contains('fx-idle')) idle(false); };
    ['pointermove', 'pointerdown', 'keydown', 'wheel'].forEach(e => addEventListener(e, input, { passive: true, capture: true }));
    setInterval(() => { if (performance.now() - last > IDLE_MS && !document.body.classList.contains('fx-idle')) idle(true); }, 1000);
  }

  /* ---- background: drifting radio nodes that link up when close ---- */
  if (!calm) {
    const cv = make('canvas'); cv.id = 'fx-bg'; cv.setAttribute('aria-hidden', 'true');
    document.body.prepend(cv);
    /* a small drawing on a CPU canvas at half resolution: a GPU canvas costs far more per frame for so few lines */
    const g = cv.getContext('2d', { willReadFrequently: true }), mouse = { x: -1e4, y: -1e4 };
    let pts = [], W = 0, H = 0, dpr = .5;
    const size = () => {
      W = innerWidth; H = innerHeight;
      cv.width = Math.ceil(W * dpr); cv.height = Math.ceil(H * dpr); cv.style.width = W + 'px'; cv.style.height = H + 'px';
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      const n = Math.min(80, Math.round(W * H / 22000));
      pts = Array.from({ length: n }, () => {
        const a = Math.random() * 6.283, s = .25 + Math.random() * .45;
        return { x: Math.random() * W, y: Math.random() * H, bx: Math.cos(a) * s, by: Math.sin(a) * s, vx: Math.cos(a) * s, vy: Math.sin(a) * s,
          ci: Math.random() * COLORS.length | 0, r: 1 + Math.random() * 1.6, ph: Math.random() * 6.28 };
      });
    };
    addEventListener('resize', size); size();
    addEventListener('pointermove', e => { mouse.x = e.clientX; mouse.y = e.clientY; }, { passive: true });
    const LINK = 130, LINK2 = LINK * LINK, LEVELS = 4;
    /* links and dots are drawn in one path per color and alpha level: a few dozen draw calls a frame, not hundreds */
    const paths = COLORS.map(() => Array.from({ length: LEVELS }, () => []));
    const flush = (draw, alpha) => {
      COLORS.forEach((c, ci) => paths[ci].forEach((list, li) => {
        if (!list.length) return;
        g.globalAlpha = alpha(li); g.beginPath();
        for (let i = 0; i < list.length; i += 4) draw(list[i], list[i + 1], list[i + 2], list[i + 3]);
        list.length = 0;
        if (draw === dot) { g.fillStyle = c; g.fill(); } else { g.strokeStyle = c; g.stroke(); }
      }));
    };
    const line = (x1, y1, x2, y2) => { g.moveTo(x1, y1); g.lineTo(x2, y2); };
    const dot = (x, y, r) => { g.moveTo(x + r, y); g.arc(x, y, r, 0, 6.283); };
    let prev = 0, running = false;
    const frame = t => {
      if (document.body.classList.contains('fx-idle')) { running = false; prev = 0; return; }
      requestAnimationFrame(frame);
      /* 30 Hz is plenty for a slow drift and halves the cost; k scales the steps so the speed stays the same */
      if (prev && t - prev < 30) return;
      const k = prev ? Math.min((t - prev) / 16.7, 4) : 1; prev = t;
      if (!document.hidden) {
        g.clearRect(0, 0, W, H);
        for (const p of pts) {
          const dx = mouse.x - p.x, dy = mouse.y - p.y, d2 = dx * dx + dy * dy;
          if (d2 < 40000) { p.vx += dx * 2e-5 * k; p.vy += dy * 2e-5 * k; }
          const damp = 1 - Math.pow(.98, k);
          p.vx += (p.bx - p.vx) * damp; p.vy += (p.by - p.vy) * damp; p.x += p.vx * k; p.y += p.vy * k;
          if (p.x < -20) p.x = W + 20; else if (p.x > W + 20) p.x = -20;
          if (p.y < -20) p.y = H + 20; else if (p.y > H + 20) p.y = -20;
        }
        g.lineWidth = 1.5;
        for (let i = 0; i < pts.length; i++) {
          const a = pts[i];
          for (let j = i + 1; j < pts.length; j++) {
            const b = pts[j], dx = a.x - b.x, dy = a.y - b.y, d2 = dx * dx + dy * dy;
            if (d2 >= LINK2) continue;
            const li = Math.min(LEVELS - 1, (1 - Math.sqrt(d2) / LINK) * LEVELS | 0);
            paths[a.ci][li].push(a.x, a.y, b.x, b.y);
          }
        }
        flush(line, li => (li + .5) / LEVELS * .22);
        for (const p of pts) {
          const li = Math.min(LEVELS - 1, (.5 + .5 * Math.sin(t / 700 + p.ph)) * LEVELS | 0);
          paths[p.ci][li].push(p.x, p.y, p.r, 0);
        }
        flush(dot, li => .45 + .35 * ((li + .5) / LEVELS * 2 - 1));
        g.globalAlpha = 1;
      }
    };
    wake = () => { if (!running) { running = true; requestAnimationFrame(frame); } };
    wake();
  }
  /* ---- party: type "ublox" outside a field, click the u-blox logo 5 times, or up up down down left right left right b a;
     with FX off it turns FX on and parties after the reload ---- */
  let party = () => { localStorage.setItem('fxoff', '0'); sessionStorage.setItem('fxparty', '1'); location.reload(); };
  /* ---- snake: the app.snake game on a canvas; arrows/WASD steer, p pause, Esc quits ---- */
  const snake = () => {
    if (document.getElementById('fx-snake')) return;
    const W = 30, H = 15, S = 20;
    const box = document.createElement('div');
    box.id = 'fx-snake';
    box.style.cssText = 'position:fixed;inset:0;z-index:9999;background:rgba(0,0,0,.75);display:flex;flex-direction:column;align-items:center;justify-content:center;color:#fff;font:14px monospace';
    const info = document.createElement('div'); info.style.margin = '8px';
    const cv = document.createElement('canvas'); cv.width = W * S; cv.height = H * S;
    cv.style.cssText = 'background:#0b1a2a;border:2px solid #ff6e59;border-radius:6px;max-width:95vw';
    const help = document.createElement('div'); help.style.margin = '8px';
    help.textContent = 'arrows/WASD steer, p pause, Esc quit, Enter restart (tap the sides on touch)';
    box.append(info, cv, help); document.body.append(box);
    const g = cv.getContext('2d');
    let body, dir, next, food, score, over, paused, timer;
    const place = () => { do { food = [Math.random() * W | 0, Math.random() * H | 0]; } while (body.some(p => p[0] === food[0] && p[1] === food[1])); };
    const start = () => { body = [[5, 7], [4, 7], [3, 7]]; dir = next = [1, 0]; score = 0; over = paused = false; place(); };
    const draw = () => {
      g.fillStyle = '#0b1a2a'; g.fillRect(0, 0, cv.width, cv.height);
      g.fillStyle = '#ffc107'; g.fillRect(food[0] * S + 4, food[1] * S + 4, S - 8, S - 8);
      body.forEach((p, i) => { g.fillStyle = i ? '#2ec4b6' : '#ff6e59'; g.fillRect(p[0] * S + 1, p[1] * S + 1, S - 2, S - 2); });
      info.textContent = over ? `Game over: score ${score}, length ${body.length} (Enter restarts)` : `Score ${score}  length ${body.length}${paused ? '  paused' : ''}`;
    };
    const step = () => {
      if (over || paused) return;
      dir = next;
      const h = [body[0][0] + dir[0], body[0][1] + dir[1]];
      if (h[0] < 0 || h[1] < 0 || h[0] >= W || h[1] >= H || body.some(p => p[0] === h[0] && p[1] === h[1])) { over = true; draw(); return; }
      body.unshift(h);
      if (h[0] === food[0] && h[1] === food[1]) { score += 10; place(); } else body.pop();
      draw();
    };
    const turn = d => { if (d[0] !== -dir[0] || d[1] !== -dir[1]) next = d; };
    const keys = { arrowup: [0, -1], w: [0, -1], arrowdown: [0, 1], s: [0, 1], arrowleft: [-1, 0], a: [-1, 0], arrowright: [1, 0], d: [1, 0] };
    const quit = () => { clearInterval(timer); removeEventListener('keydown', key, true); box.remove(); };
    const key = e => {
      const k = e.key.toLowerCase();
      e.preventDefault(); e.stopImmediatePropagation();
      if (k === 'escape' || k === 'q') quit();
      else if (k === 'p') { paused = !paused; draw(); }
      else if (k === 'enter' && over) { start(); draw(); }
      else if (keys[k]) turn(keys[k]);
    };
    cv.addEventListener('pointerdown', e => {
      const r = cv.getBoundingClientRect(), x = (e.clientX - r.left) / r.width - .5, y = (e.clientY - r.top) / r.height - .5;
      if (over) { start(); draw(); } else turn(Math.abs(x) > Math.abs(y) ? [Math.sign(x), 0] : [0, Math.sign(y)]);
    });
    box.addEventListener('click', e => { if (e.target === box) quit(); });
    addEventListener('keydown', key, true);
    start(); draw(); timer = setInterval(step, 120);
  };
  window.ucxSnake = snake;
  const menu = (x, y) => {
    document.getElementById('fx-menu')?.remove();
    const m = document.createElement('div');
    m.id = 'fx-menu';
    m.style.cssText = `position:fixed;left:${x}px;top:${y}px;z-index:9998;background:#13263a;border:1px solid #ff6e59;border-radius:6px;padding:4px;font:13px sans-serif;box-shadow:0 4px 16px rgba(0,0,0,.5)`;
    [['\u{1F389} Party', () => party()], ['\u{1F40D} Snake', snake]].forEach(([t, f]) => {
      const b = document.createElement('div');
      b.textContent = t; b.style.cssText = 'padding:6px 14px;cursor:pointer;color:#fff;border-radius:4px';
      b.onmouseenter = () => { b.style.background = '#ff6e5944'; }; b.onmouseleave = () => { b.style.background = ''; };
      b.onclick = () => { m.remove(); f(); };
      m.append(b);
    });
    document.body.append(m);
    const fb = document.getElementById('fx-tg').getBoundingClientRect();
    if (!x && !y) { x = fb.left; y = fb.bottom + 4; }
    m.style.left = Math.max(4, Math.min(x, innerWidth - m.offsetWidth - 4)) + 'px';
    m.style.top = Math.max(4, Math.min(y, innerHeight - m.offsetHeight - 4)) + 'px';
    setTimeout(() => addEventListener('pointerdown', function off(e) { if (!m.contains(e.target)) { m.remove(); removeEventListener('pointerdown', off, true); } }, true));
  };
  {
    const codes = [['ArrowUp', 'ArrowUp', 'ArrowDown', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'ArrowLeft', 'ArrowRight', 'b', 'a'],
                   ['u', 'b', 'l', 'o', 'x'], ['s', 'n', 'a', 'k', 'e']];
    const at = codes.map(() => 0);
    addEventListener('keydown', e => {
      if (e.key.length === 1 && ['INPUT', 'SELECT', 'TEXTAREA'].includes(e.target.tagName)) return;
      const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
      codes.forEach((c, i) => {
        at[i] = k === c[at[i]] ? at[i] + 1 : (k === c[0] ? 1 : 0);
        if (at[i] === c.length) { at.fill(0); if (i === 2) snake(); else party(); }
      });
    }, true);
    let clicks = [];
    const logo = document.querySelector('svg.logo');
    if (logo) logo.addEventListener('click', () => {
      const t = performance.now();
      clicks = clicks.filter(x => t - x < 2000); clicks.push(t);
      if (clicks.length >= 5) { clicks = []; party(); }
    });
    const fx = document.getElementById('fx-tg');
    if (fx) {
      let hold = 0, held = false;
      fx.title += ' (right-click or hold: party, snake)';
      fx.addEventListener('contextmenu', e => { e.preventDefault(); menu(e.clientX, e.clientY); });
      fx.addEventListener('pointerdown', e => {
        if (e.button !== 0) return;
        held = false; hold = setTimeout(() => { held = true; menu(e.clientX, e.clientY); }, 600);
      });
      ['pointerup', 'pointerleave', 'pointercancel'].forEach(n => fx.addEventListener(n, () => clearTimeout(hold)));
      fx.addEventListener('click', e => { if (held) { held = false; e.stopImmediatePropagation(); e.preventDefault(); } }, true);
    }
  }
  if (calm) return;

  /* ---- ripple on every button, spotlight on forms and cards ---- */
  document.addEventListener('pointerdown', e => {
    const b = e.target.closest && e.target.closest('button');
    if (!b || b.disabled) return;
    const r = b.getBoundingClientRect(), d = Math.max(r.width, r.height), s = make('span', 'fx-rip');
    s.style.cssText = `width:${d}px;height:${d}px;left:${e.clientX - r.left - d / 2}px;top:${e.clientY - r.top - d / 2}px`;
    b.appendChild(s); setTimeout(() => s.remove(), 650);
  });
  document.addEventListener('pointermove', e => {
    const c = e.target.closest && e.target.closest('.card,.c');
    if (!c) return;
    const r = c.getBoundingClientRect();
    c.style.setProperty('--mx', (e.clientX - r.left) + 'px'); c.style.setProperty('--my', (e.clientY - r.top) + 'px');
  }, { passive: true });

  /* ---- sparks and a shockwave for good news ---- */
  const sparks = (el, n, colors) => {
    const r = el.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2;
    for (let i = 0; i < n; i++) {
      const s = make('span', 'fx-spark'), a = Math.random() * 6.283, v = 30 + Math.random() * 60;
      s.style.left = x + 'px'; s.style.top = y + 'px'; s.style.background = colors[i % colors.length];
      s.style.boxShadow = `0 0 6px ${colors[i % colors.length]}`;
      document.body.appendChild(s);
      s.animate([{ transform: 'translate(0,0) scale(1)', opacity: 1 },
                 { transform: `translate(${Math.cos(a) * v}px,${Math.sin(a) * v + 20}px) scale(.2)`, opacity: 0 }],
                { duration: 700 + Math.random() * 400, easing: 'cubic-bezier(.2,.9,.3,1)' }).onfinish = () => s.remove();
    }
  };
  const shock = el => {
    const r = el.getBoundingClientRect(), w = make('span', 'fx-wave');
    w.style.left = (r.left + 6) + 'px'; w.style.top = (r.top + r.height / 2) + 'px';
    document.body.appendChild(w); setTimeout(() => w.remove(), 950);
  };

  /* ---- packets travelling on the live links of the system diagram ---- */
  let dgSig = '';
  const packets = () => {
    const dg = document.getElementById('dg');
    if (!dg) return;
    const on = [...dg.querySelectorAll('line.on')], sig = on.map(l => l.id).join() + '|' + (dg.firstChild ? dg.childElementCount : 0);
    if (sig === dgSig && dg.querySelector('.fx-pk')) return;
    dgSig = sig;
    dg.querySelectorAll('.fx-pk').forEach(p => p.remove());
    on.forEach((l, i) => {
      const a = `${l.getAttribute('x1')} ${l.getAttribute('y1')}`, b = `${l.getAttribute('x2')} ${l.getAttribute('y2')}`;
      [[a, b, 0], [b, a, .9], [a, b, 1.8]].forEach(([p, q, delay]) => {
        const c = document.createElementNS(SVG, 'circle'), m = document.createElementNS(SVG, 'animateMotion');
        c.setAttribute('r', '4'); c.setAttribute('class', 'fx-pk'); c.setAttribute('style', l.getAttribute('style') || '');
        m.setAttribute('dur', '2.7s'); m.setAttribute('repeatCount', 'indefinite'); m.setAttribute('begin', (delay + i * .2) + 's');
        m.setAttribute('path', `M ${p} L ${q}`); m.setAttribute('keyPoints', '0;1'); m.setAttribute('keyTimes', '0;1');
        m.setAttribute('calcMode', 'spline'); m.setAttribute('keySplines', '.4 0 .6 1');
        c.appendChild(m); dg.insertBefore(c, dg.querySelector('g'));
      });
    });
  };

  /* ---- watch the page: animate what it adds and what turns ok ---- */
  const chipText = {};
  const seen = n => n.nodeType === 1 && !(n.classList && (n.classList.contains('fx-rip') || n.classList.contains('fx-pk') ||
                                                          n.classList.contains('fx-spark') || n.classList.contains('fx-wave')));
  const added = n => {
    const cl = n.classList;
    if (cl.contains('blk')) cl.add('fx-in');
    else if (cl.contains('card') && n.parentElement && n.parentElement.id === 'steps') {
      n.style.setProperty('--i', [...n.parentElement.children].indexOf(n)); cl.add('fx-in');
    } else if (n.parentElement && n.parentElement.id === 'fit') {
      const i = [...n.parentElement.children].indexOf(n);
      n.style.setProperty('--i', i); cl.add('fx-in');
      if (cl.contains('v') && cl.contains('ok')) setTimeout(() => sparks(n, 6, ['#66d17a']), i * 40 + 300);
    } else if (cl.contains('chip')) {
      const key = n.textContent.split(' ')[0], prev = chipText[key];
      if (prev !== undefined && prev !== n.textContent) { cl.add('fx-pop'); if (cl.contains('on')) sparks(n, 10, COLORS); }
      chipText[key] = n.textContent;
    }
  };
  const changed = n => {
    const cl = n.classList;
    if (cl.contains('badge')) {
      const b = n.closest('.blk');
      if (cl.contains('ok')) { if (b) b.classList.add('fx-ok'); sparks(n, 8, ['#66d17a', '#29b6f6', '#ffd54a']); }
      else if (cl.contains('bad') && b) b.classList.add('fx-bad');
    } else if (n.id === 'st' && cl.contains('ok')) { shock(n); sparks(n, 14, COLORS); }
    else if (cl.contains('v') && cl.contains('ok')) sparks(n, 6, ['#66d17a']);
  };
  let dgDirty = false;
  new MutationObserver(list => {
    for (const m of list) {
      if (m.type === 'attributes') {
        if (m.target.closest && m.target.closest('#dg')) dgDirty = true; else changed(m.target);
        continue;
      }
      if (m.target.id === 'dg' || (m.target.closest && m.target.closest('#dg'))) {
        if ([...m.addedNodes].some(seen) || [...m.removedNodes].some(seen)) dgDirty = true;
        continue;
      }
      m.addedNodes.forEach(n => { if (seen(n)) { added(n); n.querySelectorAll && n.querySelectorAll('.v,.badge.ok').forEach(changed); } });
    }
    if (dgDirty) { dgDirty = false; packets(); }
  }).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
  packets();

  party = () => {
    document.body.classList.toggle('fx-party');
    const h = document.querySelector('h1'); if (h) for (let i = 0; i < 4; i++) setTimeout(() => sparks(h, 24, COLORS), i * 180);
  };
  if (sessionStorage.getItem('fxparty')) { sessionStorage.removeItem('fxparty'); setTimeout(party, 600); }
})();
