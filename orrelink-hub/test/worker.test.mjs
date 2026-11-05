// node test/worker.test.mjs  (no dependencies)
import assert from "node:assert/strict";
import worker, { checkBundle, parseTeamCode, normalizeCode, shapeRooms, parseFormat } from "../src/worker.js";

const kv = new Map();
const env = {
  TEAMS: { get: async (k) => kv.get(k) ?? null, put: async (k, v) => void kv.set(k, v) },
  ASSETS: { fetch: async () => new Response("asset") },
};
const bundle = new Uint8Array(617);
bundle[0] = 2; bundle[4] = 1; bundle[104] = 1; bundle[611] = 0xff;
const b64 = Buffer.from(bundle).toString("base64");

let r = await worker.fetch(new Request("https://h/api/teams", { method: "POST", body: JSON.stringify({ team: "SaveBundle:" + b64 }) }), env, {});
assert.equal(r.status, 200);
const { code } = await r.json();
assert.equal(code.length, 8);
r = await worker.fetch(new Request("https://h/api/teams", { method: "POST", body: JSON.stringify({ team: b64 }) }), env, {});
assert.equal((await r.json()).code, code, "same team gets the same code");
r = await worker.fetch(new Request("https://h/api/teams/" + code.toLowerCase()), env, {});
assert.equal((await r.json()).team, b64);
r = await worker.fetch(new Request("https://h/api/teams/ZZZZZZZZ"), env, {});
assert.equal(r.status, 404);
r = await worker.fetch(new Request("https://h/api/teams", { method: "POST", body: JSON.stringify({ team: "AAAA" }) }), env, {});
assert.equal(r.status, 400);
assert.equal(checkBundle(new Uint8Array(10)) !== null, true);
assert.equal(parseTeamCode("not base64!"), null);
assert.equal(normalizeCode("abcd-2345"), "ABCD2345");
assert.equal(normalizeCode("abcd-1234"), null);
assert.deepEqual(parseFormat("[Hoenn-U] Ash"), { tag: "Hoenn-U", format: "Hoenn Unlimited", title: "Ash" });
const rooms = shapeRooms({ sessions: [
  { name: "[Orre] A", game: "Pokemon XD: Gale of Darkness", server_id: "abc", method: "traversal", player_count: 1 },
  { name: "other", game: "Mario Kart", server_id: "x" },
] });
assert.equal(rooms.length, 1);
assert.equal(rooms[0].format, "Orre Colosseum");
assert.equal(rooms[0].title, "A");
console.log("worker tests passed");
