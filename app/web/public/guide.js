/* Getting started: a few steps that point at one part of the page each. Opens on the first visit; the ? button
   next to Demo opens it again. Kept out of index.html (the module serves that from flash). ASCII only. */
(() => {
  const STEPS = [
    [null, 'Welcome to u-connectXpressApp',
      'This page drives a u-blox u-connectXpress module (NORA-W36, NORA-B26, ALMA-W1): Wi-Fi, Bluetooth LE and ' +
      'Thread. You do not need to know AT commands. No module at hand? Run the demo: a virtual module goes through ' +
      'Wi-Fi, BLE with Channel Sounding distance, Thread, GNSS and cellular while Connections draws every link. ' +
      'Or take five short steps around the page.'],
    ['#wsc,#wsv', '1. Pick a module',
      'Connect module opens a real module on a USB serial port (Chrome or Edge). No hardware at hand? ' +
      'Virtual module starts a simulated one with the radios picked next to it. Everything below works on both.'],
    ['#demo', '2. Watch the demo',
      'Demo runs a tour of real use cases (Wi-Fi, BLE, Thread, GNSS, cellular) on a virtual module and shows every ' +
      'command it sends. The easiest way to see what the module can do.'],
    ['#vw', '3. Start from a use case',
      'Use cases shows ready-made scenarios (smart home, BLE gateway, Thread mesh, boat GNSS...) with a picture of ' +
      'the setup and buttons for each step. Toolbox is the full command list for when you know what you want.'],
    ['#cmd', '4. Or type a command',
      'Try help, then dev.info. Tab completes names; every group has an example: wifi.example, bt.example. ' +
      'Commands that change the module ask before they run.'],
    ['#out', '5. Follow what happens',
      'Log shows each command and its answer; What\'s happening on the right shows events from the radios. ' +
      'The chips at the top show Wi-Fi, BLE and socket state; click one to ask for details.'],
  ];
  const css = document.createElement('style');
  css.textContent =
    '#gd-bg{position:fixed;inset:0;z-index:9998;pointer-events:none}' +
    '#gd-box{position:fixed;z-index:9999;max-width:24rem;background:var(--card,#121a26);color:var(--tx,#e8edf4);' +
    'border:1px solid var(--ac,#29b6f6);border-radius:12px;padding:1rem 1.1rem;box-shadow:0 10px 40px #000a;' +
    'font:14px/1.45 system-ui,sans-serif}#gd-box h3{margin:0 0 .4rem;font-size:1.05rem}#gd-box p{margin:0 0 .8rem}' +
    '#gd-box .gd-r{display:flex;gap:.4rem;align-items:center}#gd-box .gd-n{flex:1;color:var(--mut,#7d8899);font-size:12px}' +
    '#gd-box .gd-go{background:var(--ac,#29b6f6);border-color:var(--ac,#29b6f6);color:#04121c;font-weight:600}' +
    '.gd-on{position:relative;z-index:9999;outline:2px solid var(--ac,#29b6f6)!important;outline-offset:3px;' +
    'box-shadow:0 0 0 9999px #0009!important;border-radius:8px}';
  document.head.appendChild(css);

  let box, at = 0, lit = [];
  const unlight = () => { lit.forEach(e => e.classList.remove('gd-on')); lit = []; };

  function place(target) {
    const r = target ? target.getBoundingClientRect() : null, w = box.offsetWidth, h = box.offsetHeight;
    let x = (innerWidth - w) / 2, y = (innerHeight - h) / 2;
    if (r) {
      x = Math.min(Math.max(8, r.left), innerWidth - w - 8);
      y = r.bottom + 14 + h < innerHeight ? r.bottom + 14 : Math.max(8, r.top - h - 14);
    }
    box.style.left = x + 'px';
    box.style.top = y + 'px';
  }

  function show(i) {
    at = i;
    unlight();
    const [sel, title, text] = STEPS[i];
    const found = sel ? [...document.querySelectorAll(sel)].filter(e => e.offsetParent) : [];
    if (!box) {
      box = document.createElement('div');
      box.id = 'gd-box';
      box.setAttribute('role', 'dialog');
      document.body.appendChild(box);
    }
    box.innerHTML = '<h3></h3><p></p><div class="gd-r"><span class="gd-n"></span><button class="gd-x">Skip</button>' +
      (i ? '<button class="gd-b">Back</button>' : '<button class="gd-demo gd-go">Run the demo</button>') +
      '<button class="gd-go gd-nx"></button></div>';
    box.querySelector('h3').textContent = title;
    box.querySelector('p').textContent = text;
    box.querySelector('.gd-n').textContent = (i + 1) + ' / ' + STEPS.length;
    box.querySelector('.gd-nx').textContent = i === STEPS.length - 1 ? 'Done' : i ? 'Next' : 'Show me around';
    box.querySelector('.gd-x').onclick = close;
    if (i) box.querySelector('.gd-b').onclick = () => show(i - 1);
    box.querySelector('.gd-nx').onclick = () => (i === STEPS.length - 1 ? close() : show(i + 1));
    if (!i) box.querySelector('.gd-demo').onclick = runDemo;
    found.forEach(e => { e.classList.add('gd-on'); lit.push(e); });
    if (found[0]) found[0].scrollIntoView({ block: 'nearest' });
    place(found[0]);
    box.querySelector(i ? '.gd-nx' : '.gd-demo').focus();
  }

  function close() {
    unlight();
    if (box) box.remove();
    box = null;
    localStorage.setItem('ucxguide', '1');
  }

  /* Virtual module (on the site), then the Connections view and the demo once the module answers */
  function runDemo() {
    close();
    const v = document.getElementById('wsv'), go = () => {
      const c = document.querySelector('#vw button[data-v="2"]'), d = document.getElementById('demo');
      if (c) c.click();
      if (d) d.click();
    };
    if (!v || v.hidden) { go(); return; }
    v.click();
    let n = 0;
    const t = setInterval(() => {
      if (v.hidden || ++n > 100) { clearInterval(t); if (v.hidden) setTimeout(go, 800); }
    }, 100);
  }

  document.addEventListener('keydown', e => { if (box && e.key === 'Escape') close(); });
  addEventListener('resize', () => { if (box) show(at); });

  const demo = document.getElementById('demo');
  if (demo) {
    const b = document.createElement('button');
    b.id = 'guide';
    b.textContent = '? Guide';
    b.title = 'Getting started: a short tour of this page';
    b.onclick = () => show(0);
    demo.before(b);
  }
  /* first visit: wait until the connect buttons are on the page so step 1 can point at them */
  if (!localStorage.getItem('ucxguide') && !/[?&]guide=0/.test(location.search)) {
    let n = 0;
    const t = setInterval(() => {
      if (document.getElementById('wsc') || ++n > 40) { clearInterval(t); if (!box) show(0); }
    }, 100);
  }
})();
