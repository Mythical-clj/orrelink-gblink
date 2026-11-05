// OrreLink Hub: team sharing + public room list for the OrreLink (Pokemon XD GBA netplay) launcher.
// Static pages live in ../public; this Worker serves /api/*.

const BUNDLE_SIZE = 617;            // PartyBundle in the OrreLink source: count + 6 mons + name + gender + id
const TEAM_TTL = 60 * 60 * 24 * 30; // shared teams are kept for 30 days
const CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ"; // no 0/O/1/I/L/U
const CODE_LEN = 8;

const json = (obj, status = 200, extra = {}) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store", ...extra },
  });

function b64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function bytesToB64(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s);
}

// Cheap structural check only; OrreLink itself strictly validates every team it loads.
export function checkBundle(bytes) {
  if (bytes.length !== BUNDLE_SIZE) return "A team code is exactly 617 bytes.";
  const count = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  if (count < 1 || count > 6) return "The team must have 1 to 6 Pokémon.";
  for (let slot = count; slot < 6; slot++) {
    for (let i = 0; i < 100; i++) if (bytes[4 + slot * 100 + i] !== 0) return "Unused team slots must be empty.";
  }
  if (bytes[612] > 1) return "Bad trainer gender.";
  return null;
}

// Accepts the raw base64 bundle, or the "SaveBundle:<base64>" form OrreLink uses on the wire.
export function parseTeamCode(text) {
  let s = String(text || "").trim();
  if (s.startsWith("SaveBundle:")) s = s.slice("SaveBundle:".length);
  s = s.replace(/\s+/g, "");
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(s) || s.length > 1200) return null;
  try {
    return b64ToBytes(s);
  } catch {
    return null;
  }
}

function newCode() {
  const r = new Uint8Array(CODE_LEN);
  crypto.getRandomValues(r);
  let c = "";
  for (const b of r) c += CODE_ALPHABET[b % CODE_ALPHABET.length];
  return c;
}

export function normalizeCode(raw) {
  const c = String(raw || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
  return c.length === CODE_LEN && [...c].every((ch) => CODE_ALPHABET.includes(ch)) ? c : null;
}

async function sha256Hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function postTeam(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "Send JSON like {\"team\": \"<team code>\"}." }, 400);
  }
  const bytes = parseTeamCode(body.team);
  if (!bytes) return json({ error: "That does not look like a team code." }, 400);
  const problem = checkBundle(bytes);
  if (problem) return json({ error: problem }, 400);

  // The same team always gets the same code, so re-sharing never spams the store.
  const hashKey = "h:" + (await sha256Hex(bytes));
  const existing = await env.TEAMS.get(hashKey);
  if (existing) {
    await env.TEAMS.put("t:" + existing, bytesToB64(bytes), { expirationTtl: TEAM_TTL });
    await env.TEAMS.put(hashKey, existing, { expirationTtl: TEAM_TTL });
    return json({ code: existing });
  }
  for (let attempt = 0; attempt < 5; attempt++) {
    const code = newCode();
    if (await env.TEAMS.get("t:" + code)) continue;
    await env.TEAMS.put("t:" + code, bytesToB64(bytes), { expirationTtl: TEAM_TTL });
    await env.TEAMS.put(hashKey, code, { expirationTtl: TEAM_TTL });
    return json({ code });
  }
  return json({ error: "Could not allocate a code, try again." }, 503);
}

async function getTeam(rawCode, env) {
  const code = normalizeCode(rawCode);
  if (!code) return json({ error: "Team codes are 8 letters and numbers." }, 400);
  const team = await env.TEAMS.get("t:" + code);
  if (!team) return json({ error: "That team code was not found (codes expire after 30 days)." }, 404);
  return json({ code, team });
}

// ---- rooms -----------------------------------------------------------------------------------
export function looksLikeXd(game) {
  const lower = String(game || "").toLowerCase();
  return ["gxxe01", "gale of darkness", "pokemon xd", "pokémon xd"].some((n) => lower.includes(n));
}

// OrreLink tags open rooms with the battle format: "[Orre] Name's room".
const FORMAT_TAGS = {
  "Orre": "Orre Colosseum", "OU": "OU", "Orre-U": "Orre Unlimited", "Orre-L": "Orre Limited",
  "Hoenn": "Hoenn Stadium", "Hoenn-U": "Hoenn Unlimited", "Hoenn-L": "Hoenn Limited",
  "DOU": "Doubles OU", "Realgam": "Realgam", "Realgam-C": "Realgam Classic", "Pyrite": "Pyrite",
  "Phenac": "Phenac", "Multi": "Multi",
};
export function parseFormat(name) {
  const m = String(name || "").match(/^\[([^\]]+)\]\s*/);
  if (!m) return { tag: "", format: "", title: String(name || "Room") };
  return { tag: m[1], format: FORMAT_TAGS[m[1]] || m[1], title: String(name).slice(m[0].length) || "Room" };
}

export function shapeRooms(list) {
  const rooms = [];
  for (const s of list.sessions || []) {
    if (!looksLikeXd(s.game)) continue;
    const f = parseFormat(s.name);
    rooms.push({
      name: String(s.name || "Room"),
      title: f.title,
      format: f.format,
      formatTag: f.tag,
      method: String(s.method || ""),
      code: String(s.server_id || ""),
      port: Number(s.port) || 0,
      players: Number(s.player_count) || 0,
      inGame: !!s.in_game,
      password: !!s.password,
      version: String(s.version || ""),
      region: String(s.region || ""),
    });
  }
  // waiting rooms first
  rooms.sort((a, b) => Number(a.inGame || a.players >= 2) - Number(b.inGame || b.players >= 2));
  return rooms;
}

async function getRooms(env, ctx) {
  const url = (env.INDEX_URL || "https://lobby.dolphin-emu.org") + "/v0/list";
  const cache = typeof caches !== "undefined" ? caches.default : null;
  const cacheKey = new Request(url);
  let upstream = cache ? await cache.match(cacheKey) : null;
  if (!upstream) {
    try {
      upstream = await fetch(url, { headers: { "X-Is-Dolphin": "1" } });
    } catch {
      return json({ error: "The public session list is unreachable right now." }, 502);
    }
    if (!upstream.ok) return json({ error: "The public session list is unreachable right now." }, 502);
    if (cache) {
      const copy = new Response(upstream.clone().body, upstream);
      copy.headers.set("cache-control", "public, max-age=5");
      ctx?.waitUntil(cache.put(cacheKey, copy));
    }
  }
  let data;
  try {
    data = await upstream.json();
  } catch {
    return json({ error: "The public session list sent something unexpected." }, 502);
  }
  if (data.status !== "OK") return json({ error: "The public session list is not available." }, 502);
  return json({ rooms: shapeRooms(data) });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (path === "/api/rooms" && request.method === "GET") return getRooms(env, ctx);
    if (path === "/api/teams" && request.method === "POST") return postTeam(request, env);
    const m = path.match(/^\/api\/teams\/([^/]+)$/);
    if (m && request.method === "GET") return getTeam(m[1], env);
    if (path.startsWith("/api/")) return json({ error: "Not found" }, 404);
    return env.ASSETS.fetch(request);
  },
};
