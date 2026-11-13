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
//  A. Steady polling. Commands come in cycles, each opened by a reset (in XD's battle program,
//     one cycle a frame). While cycles repeat command for command, a cycle is answered with the
//     real replies of the same cycle one lag ago: the game sees the GBA a few frames late, like
//     a controller under netplay's input delay.
//  B. Handshaked runs. A long run of writes and status checks with no read (sending the GBA its
//     program) is answered from the latest real reply that followed the same recent exchange,
//     which reproduces the GBA's steady acknowledgements. A real reply that turns out different
//     stops this at once (the model then waits, below).
//  C. Anything else waits for the real reply to that very command: new commands, reads outside
//     steady polling, a cycle that differs from the one before, the first cycles after one.
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

  explicit JoyBusLagModel(uint64_t lag_ticks) : m_lag(lag_ticks) {}

  // The game sends command |cmd| (with |word| when it is a write) at emulated time |ticks|.
  // Returns its index, to be given to Decide once.
  uint32_t Command(uint64_t ticks, uint8_t cmd, uint32_t word)
  {
    Entry e;
    e.ticks = ticks;
    e.cmd = cmd;
    e.word = cmd == kWrite ? word : 0;
    if (cmd == kReset || m_cycles.empty())
      OpenCycle(static_cast<uint32_t>(m_entries.size()));
    e.cycle = static_cast<uint32_t>(m_cycles.size() - 1);
    e.pos = static_cast<uint32_t>(m_entries.size()) - m_cycles.back().first;
    e.run = (cmd == kStatus || cmd == kWrite) ? m_run + 1 : 0;
    m_run = e.run;
    m_entries.push_back(e);
    return static_cast<uint32_t>(m_entries.size() - 1);
  }

  // The reply the game gets for command |k| (the latest one given to Command).
  Reply Decide(uint32_t k, const RealFn& real, How* how_out = nullptr)
  {
    Entry& e = m_entries[k];
    // Everything older than the lag is known on every machine from here on.
    while (m_horizon < k && m_entries[m_horizon].ticks + m_lag <= e.ticks)
    {
      Learn(m_horizon, real);
      IndexContext(m_horizon);
      m_horizon++;
    }

    How how = How::Waited;
    Reply out;
    if (!TryLagged(k, real, &out))
    {
      if (TryPredicted(k, &out))
        how = How::Predicted;
      else
        out = Learn(k, real);
    }
    else
    {
      how = How::Lagged;
    }
    e.virt = out;
    if (how_out)
      *how_out = how;
    return out;
  }

private:
  static constexpr uint8_t kStatus = 0x00, kRead = 0x14, kWrite = 0x15, kReset = 0xFF;
  static constexpr uint32_t kMinRun = 8;  // writes and status checks in a row before B applies
  static constexpr uint32_t kSteadyCycles = 3;  // like cycles answered for real before A applies

  struct Entry
  {
    uint64_t ticks = 0;
    uint32_t word = 0;
    uint32_t cycle = 0, pos = 0, run = 0;
    uint8_t cmd = 0;
    Reply real, virt;
  };
  struct Cycle
  {
    uint32_t first = 0;  // index of its first command
    uint32_t count = 0;
    bool differs = false;  // from the epoch's first cycle
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

  void OpenCycle(uint32_t first)
  {
    if (!m_cycles.empty())
    {
      // The cycle that just ended: one that differs from its epoch's first, in content or in
      // length, ends the epoch. The next cycle starts a new one and is answered for real.
      const uint32_t c = static_cast<uint32_t>(m_cycles.size() - 1);
      Cycle& done = m_cycles.back();
      done.count = first - done.first;
      if (c > m_epoch && (done.differs || done.count != m_cycles[m_epoch].count))
        m_epoch = c + 1;
    }
    Cycle next;
    next.first = first;
    m_cycles.push_back(next);
  }

  // A: the same position of the latest cycle of this epoch that is older than the lag.
  bool TryLagged(uint32_t k, const RealFn& real, Reply* out)
  {
    const Entry& e = m_entries[k];
    const uint32_t c = e.cycle;
    if (c <= m_epoch || m_cycles[c].differs)
      return false;
    const Cycle& ref = m_cycles[m_epoch];
    const Entry* same = e.pos < ref.count ? &m_entries[ref.first + e.pos] : nullptr;
    if (!same || same->cmd != e.cmd || same->word != e.word)
    {
      m_cycles[c].differs = true;
      return false;
    }
    // The first cycles of an epoch are answered for real: only polling that has settled lags.
    if (c < m_epoch + kSteadyCycles)
      return false;
    // Cycles m_epoch..c-1 are all like the first one (or the epoch would have ended), so their
    // command at this position is at the same offset.
    uint32_t h = c - 1;
    while (h > m_epoch && m_cycles[h].first + e.pos >= m_horizon)
      h--;
    const uint32_t source = m_cycles[h].first + e.pos;
    // A cycle that comes back after a long gap (the same opening as minutes ago) is not polling.
    if (e.ticks - m_entries[source].ticks > 3 * m_lag)
    {
      m_cycles[c].differs = true;
      return false;
    }
    *out = Learn(source, real);
    return true;
  }

  static uint64_t Mix(uint64_t h, uint64_t v)
  {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
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

  bool TryPredicted(uint32_t k, Reply* out)
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
    *out = past.real;
    return true;
  }

  const uint64_t m_lag;
  std::vector<Entry> m_entries;
  std::vector<Cycle> m_cycles;
  uint32_t m_epoch = 0;    // first cycle of the current run of like cycles
  uint32_t m_horizon = 0;  // commands before this one are older than the lag and known
  uint32_t m_run = 0;
  std::unordered_map<uint64_t, uint32_t> m_context;
};
}  // namespace SerialInterface
