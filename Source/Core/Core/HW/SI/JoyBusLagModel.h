// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What a netplay room tells the game a real GBA said (SI_DeviceGBALink).
//
// A real GBA answers on one machine only, and every other machine learns the answer one network
// trip later. Waiting for each answer would cost a trip per command, and the game sends hundreds
// of commands a second. So every machine runs this model for each real-GBA port instead: it is
// given the game's commands (the same on every machine, emulation being deterministic) and the
// GBA's real replies as they arrive from the port's owner, and it decides the reply the game
// gets. Every machine decides the same, because a decision only ever uses real replies older
// than a fixed span of emulated time (the lag), which every machine waits for.
//
// The game's command stream goes to the real GBA untouched and on time, on the owner's machine;
// only what the game hears back is arranged here, three ways:
//
//  A. Repetition. While the game repeats itself, command for command (XD's battle program is
//     polled with the same nine commands every frame; a GBA being waited for is asked for its
//     status over and over), a command is answered with the real reply to the same command of
//     the latest repetition older than the lag. The game sees the GBA a few
//     frames late, like a controller under netplay's input delay. This is safe by the bus's own
//     rules: a flag the GBA raised stays raised until the game acts on it, and the game acting
//     differently ends the repetition.
//  B. The program upload. A long run of writes and status checks with no read, in which the
//     cartridge acknowledges each word by flipping a flag, is answered from the latest real
//     reply that followed the same recent exchange, which reproduces those acknowledgements.
//     A real reply that turns out different stops this at once (the model then waits, below).
//  (The end of the program upload, from its last 32 writes on, is not guessed: it waits too.)
//  C. Anything else waits for the real reply to that very command: new commands, their data,
//     the first repetitions after one.
//  D. (before C) Quiet replies: a write or a status to which the GBA has said "nothing for you"
//     (no receive flag) eight times running is answered the same, since a flag it raises later
//     stays raised and the game sees it one lag later. Replayed against five local sessions
//     this takes the commands that wait from about 10,000 to about 1,500 in a hundred thousand.
//
// No Dolphin types in here, so that it can be run against a recorded session (JoyTrace.txt).

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace SerialInterface
{
class JoyBusLagModel
{
public:
  struct Reply
  {
    int8_t len = -2;  // -2: not known yet, -1: the GBA did not answer, else the byte count
    std::array<uint8_t, 5> data{};
    bool operator==(const Reply& o) const { return len == o.len && data == o.data; }
  };
  enum class How : uint8_t { Waited, Lagged, Predicted };

  // real(index) returns the real reply to command |index|, waiting for it if it has not arrived.
  using RealFn = std::function<Reply(uint32_t index)>;

  // |wait_only|: no repetition or upload shortcuts, every command waits for its real reply.
  explicit JoyBusLagModel(uint64_t lag_ticks, bool wait_only = false, bool quiet = true)
      : m_lag(lag_ticks), m_wait_only(wait_only), m_quiet_on(quiet)
  {
  }

  // The game sends command |cmd| (with |word| when it is a write) at emulated time |ticks|.
  // Returns its index, to be given to Decide once.
  uint32_t Command(uint64_t ticks, uint8_t cmd, uint32_t word)
  {
    const uint32_t k = static_cast<uint32_t>(m_entries.size());
    Entry e;
    e.ticks = ticks;
    e.cmd = cmd;
    e.word = cmd == kWrite ? word : 0;
    e.run = (cmd == kStatus || cmd == kWrite) ? m_run + 1 : 0;
    m_run = e.run;
    // The program upload is a fixed 26891 writes after the last reset. Its end is the one moment
    // nothing before it can predict (the GBA raises its receive flag and drops it again within
    // about 75 ms if the game has not read), so the commands around it all wait for the real reply.
    if (cmd == kReset)
      m_upload_writes = 0;
    else if (cmd == kWrite && ++m_upload_writes == kUploadWrites - kEndApproach)
      m_end_left = kEndWindow;
    const bool at_end = m_end_left > 0;
    if (at_end)
      m_end_left--;
    e.at_end = at_end;
    m_entries.push_back(e);
    // For every period, how many commands in a row (ending here) repeat the one a period back.
    for (uint32_t p = 1; p <= kMaxPeriod; p++)
    {
      const bool same =
          k >= p && m_entries[k - p].cmd == e.cmd && m_entries[k - p].word == e.word;
      m_repeat[p] = same ? m_repeat[p] + 1 : 0;
    }
    return k;
  }

  // The reply the game gets for command |k| (the latest one given to Command).
  Reply Decide(uint32_t k, const RealFn& real, How* how_out = nullptr)
  {
    Entry& e = m_entries[k];
    // Everything older than the lag is known on every machine from here on.
    while (m_horizon < k && m_entries[m_horizon].ticks + m_lag <= e.ticks)
    {
      Learn(m_horizon, real);
      NoteQuiet(m_horizon);
      IndexContext(m_horizon);
      m_horizon++;
    }

    How how = How::Waited;
    Reply out;
    e.source = k;
    if (e.at_end)
      out = Learn(k, real);
    else if (!m_wait_only && TryLagged(k, &out, &e.source))
      how = How::Lagged;
    else if (!m_wait_only && TryPredicted(k, &out))
      how = How::Predicted;
    else if (!m_wait_only && m_quiet_on && TryQuiet(k, &out))
      how = How::Predicted;
    else
      out = Learn(k, real);
    e.virt = out;
    if (how_out)
      *how_out = how;
    return out;
  }

private:
  static constexpr uint8_t kStatus = 0x00, kRead = 0x14, kWrite = 0x15, kReset = 0xFF;
  static constexpr uint32_t kMaxPeriod = 64;  // commands in the longest repetition looked for
  static constexpr uint32_t kRepeats = 3;     // repetitions answered for real before A applies
  static constexpr uint32_t kMinRun = 8;      // writes and status checks in a row before B applies
  static constexpr uint32_t kUploadWrites = 26891;  // writes in a program upload, after a reset
  static constexpr uint32_t kEndApproach = 32;      // the last writes of it that already wait
  static constexpr uint32_t kEndWindow = 220;       // commands that wait, from there on

  struct Entry
  {
    uint64_t ticks = 0;
    uint32_t word = 0;
    uint32_t run = 0;
    uint32_t source = 0;  // the command whose real reply the game was given for this one
    uint8_t cmd = 0;
    bool at_end = false;  // inside the end of a program upload: waits for the real reply
    Reply real, virt;
  };

  const Reply& Learn(uint32_t i, const RealFn& real)
  {
    Entry& e = m_entries[i];
    if (e.real.len == -2)
    {
      e.real = real(i);
      if (e.real.len == -2)
        e.real.len = -1;
    }
    return e.real;
  }

  // A: the same command of the latest repetition that is older than the lag. A repetition that
  // has only just begun has no reply that old yet: its latest real reply (which every machine
  // waited for) is held until one is.
  bool TryLagged(uint32_t k, Reply* out, uint32_t* source) const
  {
    const Entry& e = m_entries[k];
    for (uint32_t p = 1; p <= kMaxPeriod; p++)
    {
      // A status asked again straight away may be held from the second time on: nothing the
      // game did in between could have changed it.
      const uint32_t needed = (p == 1 && e.cmd == kStatus) ? 1 : kRepeats * p;
      if (m_repeat[p] < needed)
        continue;
      if (!MayLag(k, p))
        continue;
      // Commands k - m_repeat[p] - p + 1 .. k repeat with this period.
      const uint32_t first = k + 1 - m_repeat[p] - p;
      uint32_t j = k - p;
      while (j >= m_horizon && j >= first + p)
        j -= p;
      uint32_t from;
      if (j < m_horizon && j >= first)
      {
        from = j;
        *out = m_entries[j].real;
      }
      else
      {
        const Entry& last = m_entries[k - p];
        if (last.source < first)
          continue;  // what it was told predates the repetition
        from = last.source;
        *out = last.virt;
      }
      // Kept up without a long pause; after one, what the GBA said before it is no guide.
      if (e.ticks - m_entries[from].ticks > 3 * m_lag)
        continue;
      *source = from;
      return true;
    }
    return false;
  }

  // Which repetitions may be answered late. One command over and over, unless it is a read
  // (every read takes the GBA's next word). Or a round that opens with a reset, like the battle
  // program's poll, in which every read answers a write or the reset of that same round: asking
  // the same thing again gets the same kind of answer. Not a stream of words being fetched or
  // sent, where each status check paces the next word and must be the real one.
  bool MayLag(uint32_t k, uint32_t p) const
  {
    if (p == 1)
      return m_entries[k].cmd != kRead;
    uint32_t reset = p;
    for (uint32_t i = 0; i < p; i++)
    {
      if (m_entries[k - i].cmd == kReset)
        reset = i;  // ends up at the earliest reset of the round
    }
    if (reset == p)
      return false;
    bool asked = false;
    for (uint32_t n = 0; n < p; n++)
    {
      // The round in order, from that reset on, wrapping around.
      const uint32_t back = (reset + p - n) % p;
      const uint8_t cmd = m_entries[k - back].cmd;
      if (cmd == kReset || cmd == kWrite)
        asked = true;
      else if (cmd == kRead)
      {
        if (!asked)
          return false;
        asked = false;
      }
    }
    return true;
  }

  static uint64_t Mix(uint64_t h, uint64_t v)
  {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
  }

  // The reply carries the GBA's status with the second general-purpose flag (0x20) set.
  static bool Handshaking(uint8_t cmd, const Reply& r)
  {
    if (cmd == kWrite)
      return r.len >= 1 && (r.data[0] & 0x20) != 0;
    if (cmd == kStatus)
      return r.len >= 3 && (r.data[2] & 0x20) != 0;
    return false;
  }

  // The command and the three exchanges before it, as the game saw them (written words left
  // out: an acknowledgement does not depend on what was sent).
  bool ContextKey(uint32_t i, uint64_t* key) const
  {
    if (i < 3)
      return false;
    uint64_t h = m_entries[i].cmd;
    for (uint32_t back = 1; back <= 3; back++)
    {
      const Entry& p = m_entries[i - back];
      uint64_t v = p.cmd;
      v = (v << 8) | static_cast<uint8_t>(p.virt.len);
      for (int b = 0; b < 5; b++)
        v = (v << 8) | (p.cmd == kRead && b < 4 ? 0 : p.virt.data[b]);
      h = Mix(h, v);
    }
    *key = h;
    return true;
  }

  void IndexContext(uint32_t i)
  {
    uint64_t key;
    if (m_entries[i].run >= 1 && ContextKey(i, &key))
      m_context[key] = i;
  }

  // B: what the cartridge said the last time the same exchange led up to this command.
  bool TryPredicted(uint32_t k, Reply* out) const
  {
    const Entry& e = m_entries[k];
    if (e.run < kMinRun)
      return false;
    uint64_t key;
    if (!ContextKey(k, &key))
      return false;
    const auto it = m_context.find(key);
    if (it == m_context.end())
      return false;
    const Entry& past = m_entries[it->second];
    // The last time this happened the GBA did not say what the game was told: stop guessing.
    if (!(past.real == past.virt))
      return false;
    // Only the exchange this is meant for: a cartridge taking its program acknowledges every
    // word by flipping a flag, with the second general-purpose flag held high throughout.
    // Anywhere else (a battle program sent a burst of text, or told to wait) the same recent
    // exchange does not promise the same answer, and a wrong "the GBA has a word for you" makes
    // the game read one that is not there.
    bool handshake = Handshaking(e.cmd, past.real);
    for (uint32_t back = 1; back <= 3; back++)
      handshake = handshake && Handshaking(m_entries[k - back].cmd, m_entries[k - back].virt);
    if (!handshake)
      return false;
    *out = past.real;
    return true;
  }

  // D: quiet replies. A write or a status to which the GBA has said "nothing for you" (no
  // receive flag) many times running, and still does, is answered the same without waiting: if
  // it does raise the flag, the flag stays raised, and the game sees it one lag later.
  static constexpr uint32_t kQuietRuns = 8;
  struct Quiet
  {
    Reply last;
    uint32_t same = 0;
    uint32_t cooldown = 0;
  };
  static bool HasRecv(uint8_t cmd, const Reply& r)
  {
    if (cmd == kWrite)
      return r.len < 1 || (r.data[0] & 0x08) != 0;
    return r.len < 3 || (r.data[2] & 0x08) != 0;
  }
  void NoteQuiet(uint32_t i)
  {
    const Entry& e = m_entries[i];
    if (e.cmd != kWrite && e.cmd != kStatus)
      return;
    Quiet& q = m_quiet[e.cmd == kWrite ? 1 : 0];
    if (q.cooldown)
      q.cooldown--;
    if (e.real == q.last)
      q.same++;
    else
    {
      q.last = e.real;
      q.same = 1;
    }
    if (!(e.virt == e.real))
      q.cooldown = 32;  // it guessed wrong once: wait for a while
  }
  bool TryQuiet(uint32_t k, Reply* out) const
  {
    const Entry& e = m_entries[k];
    if (e.cmd != kWrite && e.cmd != kStatus)
      return false;
    const Quiet& q = m_quiet[e.cmd == kWrite ? 1 : 0];
    if (q.same < kQuietRuns || q.cooldown || q.last.len < 1 || HasRecv(e.cmd, q.last))
      return false;
    *out = q.last;
    return true;
  }
  std::array<Quiet, 2> m_quiet{};

  const uint64_t m_lag;
  const bool m_wait_only;
  const bool m_quiet_on;
  std::vector<Entry> m_entries;
  uint32_t m_horizon = 0;  // commands before this one are older than the lag and known
  uint32_t m_run = 0;
  uint32_t m_upload_writes = 0;
  uint32_t m_end_left = 0;
  std::array<uint32_t, kMaxPeriod + 1> m_repeat{};
  std::unordered_map<uint64_t, uint32_t> m_context;
};
}  // namespace SerialInterface
