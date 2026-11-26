// Offline cache for the installer: after the first visit everything (page, flashing
// library, font, firmware) is served from the browser cache when there is no internet.
const CACHE = "pokesigner-0.7.1c";
const FILES = [
 "./",
 "index.html",
 "bip39-en.js",
 "vendor/press-start-2p/font.css",
 "vendor/press-start-2p/files/press-start-2p-latin-400-normal.woff2",
 "vendor/press-start-2p/files/press-start-2p-latin-400-normal.woff",
 "vendor/esp-web-tools/cdc-acm-CgpHa-zp.js",
 "vendor/esp-web-tools/ch34x-O7hnS6Rn.js",
 "vendor/esp-web-tools/cp210x-CD3hAcBr.js",
 "vendor/esp-web-tools/esp32-BlyKoTgy.js",
 "vendor/esp-web-tools/esp32c2-Bmu-jpVE.js",
 "vendor/esp-web-tools/esp32c3-81Rr-aQE.js",
 "vendor/esp-web-tools/esp32c5-Bq6mXcb5.js",
 "vendor/esp-web-tools/esp32c6-yQoBKrxY.js",
 "vendor/esp-web-tools/esp32c61-DmDn041x.js",
 "vendor/esp-web-tools/esp32h2-B8bZhHv5.js",
 "vendor/esp-web-tools/esp32h4-KyNsS-mo.js",
 "vendor/esp-web-tools/esp32p4-o74i42i3.js",
 "vendor/esp-web-tools/esp32p4-rev1-DEdUM_9Q.js",
 "vendor/esp-web-tools/esp32s2-O1Tyu6IT.js",
 "vendor/esp-web-tools/esp32s3-WXxMoV8x.js",
 "vendor/esp-web-tools/esp32s31-Ef8NLP2_.js",
 "vendor/esp-web-tools/esp8266-DsDItw_h.js",
 "vendor/esp-web-tools/ftdi-E6n0R6W9.js",
 "vendor/esp-web-tools/index-rScOpSrP.js",
 "vendor/esp-web-tools/install-button.js",
 "vendor/esp-web-tools/install-dialog-ChRWE6GU.js",
 "vendor/esp-web-tools/no-port-picked-DPUnbpty.js",
 "vendor/esp-web-tools/port-3AmrEwYL.js",
 "vendor/esp-web-tools/styles-BiOOVrm2.js",
 "assets/signy-c3.gif",
 "assets/signy-c6.gif",
 "firmware/c3/manifest.json",
 "firmware/c3/pokesigner-esp32c3.bin",
 "firmware/c6/manifest.json",
 "firmware/c6/pokesigner-esp32c6.bin",
 "firmware/tdisplay/manifest.json",
 "firmware/tdisplay/pokesigner-tdisplay.bin"
];
self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});
self.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});
// network first (to get updates), cache when offline
self.addEventListener("fetch", (e) => {
  if (e.request.method !== "GET" || new URL(e.request.url).origin !== location.origin) return;
  e.respondWith(fetch(e.request).then((r) => {
    if (r.ok) { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(e.request, copy)); }
    return r;
  }).catch(() => caches.match(e.request, { ignoreSearch: true })));
});
