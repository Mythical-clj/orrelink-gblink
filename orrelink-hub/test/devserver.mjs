// Local preview without wrangler: node test/devserver.mjs  ->  http://localhost:8787
import http from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join } from "node:path";
import worker from "../src/worker.js";

const kv = new Map();
const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".png": "image/png", ".ico": "image/x-icon" };
const pub = new URL("../public/", import.meta.url).pathname;
const env = {
  TEAMS: { get: async (k) => kv.get(k) ?? null, put: async (k, v) => void kv.set(k, v) },
  INDEX_URL: process.env.INDEX_URL || "https://lobby.dolphin-emu.org",
  ASSETS: { fetch: async (req) => {
    const p = new URL(req.url).pathname;
    const file = /\.\w+$/.test(p) ? p : "/index.html";
    try { return new Response(await readFile(join(pub, file)), { headers: { "content-type": types[extname(file)] || "application/octet-stream" } }); }
    catch { return new Response("not found", { status: 404 }); }
  } },
};
http.createServer(async (req, res) => {
  const chunks = []; for await (const c of req) chunks.push(c);
  const r = await worker.fetch(new Request("http://localhost:8787" + req.url, { method: req.method, headers: req.headers, body: ["GET", "HEAD"].includes(req.method) ? undefined : Buffer.concat(chunks) }), env, {});
  res.writeHead(r.status, Object.fromEntries(r.headers)); res.end(Buffer.from(await r.arrayBuffer()));
}).listen(8787, () => console.log("http://localhost:8787"));
