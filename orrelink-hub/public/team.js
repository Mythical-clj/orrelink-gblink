// Gen 3 save -> OrreLink team code (PartyBundle, 617 bytes), entirely in the browser.
// Mirrors UICommon/XDNetplay/{Gen3Save,PartyBundle}.cpp. OrreLink itself re-validates every
// team strictly when it loads one, so this is a convenience, never the authority.

const SECTION = 4096, PER_BLOCK = 14, BLOCK = SECTION * PER_BLOCK;
const FOOTER = 0xff4, SIGNATURE = 0x08012025;
export const BUNDLE_SIZE = 617;

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

function section0Length(game) { return game === "rs" ? 2192 : game === "frlg" ? 3876 : 3884; }

function checksum(b, base, len) {
  let sum = 0;
  for (let i = 0; i + 4 <= len; i += 4) sum = (sum + u32(b, base + i)) >>> 0;
  return ((sum & 0xffff) + (sum >>> 16)) & 0xffff;
}

function blockInfo(b, blk) {
  const pos = new Array(PER_BLOCK).fill(-1);
  let index = -1, valid = true;
  for (let slot = 0; slot < PER_BLOCK; slot++) {
    const o = blk * BLOCK + slot * SECTION + FOOTER;
    const id = u16(b, o), sig = u32(b, o + 4), idx = u32(b, o + 8);
    if (sig !== SIGNATURE || id >= PER_BLOCK || pos[id] !== -1) { valid = false; continue; }
    pos[id] = slot;
    if (index === -1) index = idx; else if (index !== idx) valid = false;
  }
  return { pos, index, valid };
}

// Gen 3 text for the characters a trainer name can hold (Western games).
function decodeChar(c) {
  if (c >= 0xbb && c <= 0xd4) return String.fromCharCode(65 + c - 0xbb);
  if (c >= 0xd5 && c <= 0xee) return String.fromCharCode(97 + c - 0xd5);
  if (c >= 0xa1 && c <= 0xaa) return String.fromCharCode(48 + c - 0xa1);
  const punct = { 0x00: " ", 0xab: "!", 0xac: "?", 0xad: ".", 0xae: "-", 0xb4: "'" };
  return punct[c] ?? "·";
}

function checkMon(mon) {
  const pid = u32(mon, 0), key = (pid ^ u32(mon, 4)) >>> 0;
  let sum = 0;
  for (let i = 0; i < 48; i += 4) {
    const v = (u32(mon, 0x20 + i) ^ key) >>> 0;
    sum += (v & 0xffff) + (v >>> 16);
  }
  return (sum & 0xffff) === u16(mon, 0x1c);
}

// Returns { bundle: Uint8Array(617), game, trainer, count } or throws Error with a plain message.
export function extractTeam(save) {
  const b = save instanceof Uint8Array ? save : new Uint8Array(save);
  if (b.length < 2 * BLOCK) throw new Error("That file is too small to be a Gen 3 save (it should be 128 KB).");
  const info = [blockInfo(b, 0), blockInfo(b, 1)];
  let active = -1;
  if (info[0].valid && info[1].valid) active = info[1].index > info[0].index ? 1 : 0;
  else if (info[1].valid) active = 1;
  else if (info[0].valid) active = 0;
  if (active < 0) throw new Error("No valid save slot was found. Is this a Pokémon Ruby, Sapphire, FireRed, LeafGreen or Emerald save?");

  const at = (id) => active * BLOCK + info[active].pos[id] * SECTION;
  const s0 = at(0), s1 = at(1);
  const marker = u32(b, s0 + 0xac);
  const game = marker === 0 ? "rs" : marker === 1 ? "frlg" : "emerald";
  if (checksum(b, s0, section0Length(game)) !== u16(b, s0 + FOOTER + 2) ||
      checksum(b, s1, 3968) !== u16(b, s1 + FOOTER + 2)) {
    throw new Error("The save's checksum does not match, so it looks damaged. Try reading the cartridge again.");
  }

  const [countOff, dataOff] = game === "frlg" ? [0x34, 0x38] : [0x234, 0x238];
  const count = u32(b, s1 + countOff);
  if (count < 1 || count > 6) throw new Error("The party must hold 1 to 6 Pokémon (found " + count + ").");

  const bundle = new Uint8Array(BUNDLE_SIZE);
  bundle[0] = count;
  for (let i = 0; i < count; i++) {
    const mon = b.slice(s1 + dataOff + i * 100, s1 + dataOff + (i + 1) * 100);
    if (u32(mon, 0) === 0 && u32(mon, 4) === 0) throw new Error("Party slot " + (i + 1) + " is empty.");
    if (!checkMon(mon)) throw new Error("Party slot " + (i + 1) + " is damaged (its checksum does not match).");
    bundle.set(mon, 4 + i * 100);
  }
  // trainer name: up to the first 0xFF, padded with 0xFF, terminator forced
  let ended = false, name = "";
  for (let i = 0; i < 7; i++) {
    const c = b[s0 + i];
    ended = ended || c === 0xff;
    bundle[604 + i] = ended ? 0xff : c;
    if (!ended) name += decodeChar(c);
  }
  bundle[611] = 0xff;
  if (!name) throw new Error("The save's trainer name is empty.");
  const gender = b[s0 + 8];
  if (gender > 1) throw new Error("The save's trainer gender is invalid.");
  bundle[612] = gender;
  bundle.set(b.slice(s0 + 0xa, s0 + 0xe), 613);
  return { bundle, game, trainer: name, count };
}

export function bundleToCode(bundle) {
  let s = "";
  for (const x of bundle) s += String.fromCharCode(x);
  return btoa(s);
}

export function codeToBundle(code) {
  let s = String(code || "").trim();
  if (s.startsWith("SaveBundle:")) s = s.slice(11);
  s = s.replace(/\s+/g, "");
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  if (out.length !== BUNDLE_SIZE) throw new Error("That is not a team code (wrong length).");
  return out;
}

// Substructure order of a party Pokémon's encrypted block, indexed by PID % 24.
const ORDERS = ["GAEM","GAME","GEAM","GEMA","GMAE","GMEA","AGEM","AGME","AEGM","AEMG","AMGE","AMEG",
                "EGAM","EGMA","EAGM","EAMG","EMGA","EMAG","MGAE","MGEA","MAGE","MAEG","MEGA","MEAG"];

// Species id (Gen 3 internal numbering) of one 100-byte party Pokémon.
export function monInfo(mon) {
  const pid = u32(mon, 0), key = (pid ^ u32(mon, 4)) >>> 0;
  const growth = ORDERS[pid % 24].indexOf("G");
  return { species: (u16(mon, 0x20 + growth * 12) ^ (key & 0xffff)) & 0xffff };
}

export function describeBundle(bundle) {
  let name = "";
  for (let i = 0; i < 7 && bundle[604 + i] !== 0xff; i++) name += decodeChar(bundle[604 + i]);
  const members = [];
  for (let i = 0; i < Math.min(bundle[0], 6); i++) members.push(monInfo(bundle.slice(4 + i * 100, 4 + (i + 1) * 100)));
  return { count: bundle[0], trainer: name, gender: bundle[612] === 1 ? "girl" : "boy", members };
}
