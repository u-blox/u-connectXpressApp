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

  /* ---- background: drifting radio nodes that link up when close ---- */
  if (!calm) {
    const cv = make('canvas'); cv.id = 'fx-bg'; cv.setAttribute('aria-hidden', 'true');
    document.body.prepend(cv);
    const g = cv.getContext('2d'), mouse = { x: -1e4, y: -1e4 };
    let pts = [], W = 0, H = 0, dpr = 1;
    const size = () => {
      dpr = 1; W = innerWidth; H = innerHeight;
      cv.width = W * dpr; cv.height = H * dpr; cv.style.width = W + 'px'; cv.style.height = H + 'px';
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      const n = Math.min(110, Math.round(W * H / 16000));
      pts = Array.from({ length: n }, () => {
        const a = Math.random() * 6.283, s = .25 + Math.random() * .45;
        return { x: Math.random() * W, y: Math.random() * H, bx: Math.cos(a) * s, by: Math.sin(a) * s, vx: Math.cos(a) * s, vy: Math.sin(a) * s,
          c: COLORS[Math.random() * COLORS.length | 0], r: 1 + Math.random() * 1.6, ph: Math.random() * 6.28 };
      });
    };
    addEventListener('resize', size); size();
    addEventListener('pointermove', e => { mouse.x = e.clientX; mouse.y = e.clientY; }, { passive: true });
    const LINK = 130, LINK2 = LINK * LINK;
    let prev = 0;
    const frame = t => {
      /* steps of a 60 Hz frame, so the motion keeps its speed on 30 Hz and 144 Hz screens */
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
        g.lineWidth = 1;
        for (let i = 0; i < pts.length; i++) {
          const a = pts[i];
          for (let j = i + 1; j < pts.length; j++) {
            const b = pts[j], dx = a.x - b.x, dy = a.y - b.y, d2 = dx * dx + dy * dy;
            if (d2 >= LINK2) continue;
            const d = Math.sqrt(d2);
            g.strokeStyle = a.c; g.globalAlpha = (1 - d / LINK) * .22; g.beginPath(); g.moveTo(a.x, a.y); g.lineTo(b.x, b.y); g.stroke();
          }
        }
        for (const p of pts) {
          g.globalAlpha = .45 + .35 * Math.sin(t / 700 + p.ph); g.fillStyle = p.c;
          g.beginPath(); g.arc(p.x, p.y, p.r, 0, 6.283); g.fill();
        }
        g.globalAlpha = 1;
      }
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
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

  /* ---- up up down down left right left right b a ---- */
  const code = ['ArrowUp', 'ArrowUp', 'ArrowDown', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'ArrowLeft', 'ArrowRight', 'b', 'a'];
  let at = 0;
  addEventListener('keydown', e => {
    at = e.key === code[at] ? at + 1 : (e.key === code[0] ? 1 : 0);
    if (at === code.length) {
      at = 0; document.body.classList.toggle('fx-party');
      const h = document.querySelector('h1'); if (h) for (let i = 0; i < 4; i++) setTimeout(() => sparks(h, 24, COLORS), i * 180);
    }
  });
})();
