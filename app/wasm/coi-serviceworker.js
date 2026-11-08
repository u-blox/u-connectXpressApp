/* Cross-origin isolation for static hosts that cannot set headers (GitHub Pages): the WebAssembly app uses
   threads, which need SharedArrayBuffer, which needs COOP/COEP. This worker adds them to every response.
   It also keeps the site offline (installable app): network first, so an online visit always gets the newest
   release, and the last copy from the cache when there is no network.
   Page side: index.html registers it and reloads once when it takes control. */
const CACHE = 'ucxapp-site';
const FILES = ['./', 'index.html', 'ucxapp.js', 'ucxapp.wasm', 'ucxapp-wasm.js', 'manifest.webmanifest', 'icon.svg',
               'fx.css', 'fx.js', 'guide.js', 'valma-w1.json', 'vnora-w36.json', 'vnora-b26-thread.json',
               'vnora-b26.json'];
const isolate = res => {
  if (res.status === 0) return res;
  const h = new Headers(res.headers);
  h.set('Cross-Origin-Opener-Policy', 'same-origin');
  h.set('Cross-Origin-Embedder-Policy', 'require-corp');
  h.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
};
self.addEventListener('install', e => {
  self.skipWaiting();
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).catch(() => {}));
});
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const r = e.request;
  if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
  const local = r.method === 'GET' && new URL(r.url).origin === self.location.origin;
  e.respondWith(fetch(r).then(res => {
    if (local && res.ok) {
      const copy = res.clone();
      e.waitUntil(caches.open(CACHE).then(c => c.put(r, copy)).catch(() => {}));
    }
    return isolate(res);
  }, err => (local ? caches.match(r, { ignoreSearch: true }) : Promise.resolve())
    .then(hit => { if (hit) return isolate(hit); throw err; })));
});