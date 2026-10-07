// Synthetic Node.js "microservice" for the KSM experiment — same memory model as CatalogService.java.
// shared catalog (fixed seed, identical in every replica) + private sessions (random) + request churn.
// Env: CATALOG_MB, PRIVATE_MB, PORT
const http = require('http');
const crypto = require('crypto');

const CATALOG_MB = parseInt(process.env.CATALOG_MB || '200', 10);
const PRIVATE_MB = parseInt(process.env.PRIVATE_MB || '50', 10);
const PORT = parseInt(process.env.PORT || '8080', 10);

// Deterministic PRNG (mulberry32) so every replica builds the same catalog
function rng(seed) {
    return () => {
        seed |= 0; seed = seed + 0x6D2B79F5 | 0;
        let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
        t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
        return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
}
const shared = rng(42);
// Flat strings via join() — `s += c` builds cons-string chains that blow up V8's heap
const text = (n) => { const c = new Array(n); for (let i = 0; i < n; i++) c[i] = String.fromCharCode(97 + Math.floor(shared() * 26)); return c.join(''); };

const catalog = [];
for (let i = 0; i < CATALOG_MB * 1024; i++) {
    catalog.push({ id: i, sku: 'SKU-' + i, title: text(48), description: text(400),
                   price: Math.floor(shared() * 100000) / 100,
                   tags: Array.from({ length: 8 }, () => Math.floor(shared() * 1000)) });
}
const sessions = [];
for (let i = 0; i < PRIVATE_MB * 16; i++) sessions.push(crypto.randomBytes(64 * 1024));
console.log(`ready: catalog=${catalog.length} sessions=${sessions.length}`);

http.createServer((req, res) => {
    const start = Math.floor(Math.random() * (catalog.length - 200));
    let sum = 0; const items = [];
    for (let i = start; i < start + 200; i++) { sum += catalog[i].price; if (items.length < 20) items.push(catalog[i].sku); }
    const s = sessions[Math.floor(Math.random() * sessions.length)];
    for (let i = 0; i < 64; i++) s[Math.floor(Math.random() * s.length)] = Math.floor(Math.random() * 256);
    const garbage = []; for (let i = 0; i < 50; i++) garbage.push(Buffer.alloc(1024));
    const body = JSON.stringify({ sum, items });
    res.writeHead(200, { 'Content-Length': Buffer.byteLength(body) });
    res.end(body);
}).listen(PORT);
