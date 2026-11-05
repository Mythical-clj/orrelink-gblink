// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Owns the GB-Link USB adapters (up to four) for the whole Dolphin process. Each adapter gets a
// Link with its own background thread:
//  * finds the adapter (hot-plug),
//  * when the GBA sits at its multiboot-wait screen, sends it the OrreLink agent,
//  * keeps the GBA in controller mode and publishes its key state (read by GBAPad),
//  * on request, pauses the controller and reads the cartridge save.
// Nothing here ever writes to a cartridge.

#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "InputCommon/GBLink/ScreenMirror.h"

namespace GBLink
{
enum class State
{
  Stopped,
  NoAdapter,
  WaitingForGba,   // adapter present, GBA not yet in multiboot-wait
  LoadingAgent,
  ControllerActive,
  AgentIdle,       // agent running, controller paused (disabled, or no pad support)
  ReadingCart,
  Error,
};

struct Status
{
  State state = State::Stopped;
  std::string text;
};

struct DumpResult
{
  bool ok = false;
  std::string error;
  std::vector<uint8_t> save;
  std::string title;
  std::string code;
  // True when only the Gen 3 trainer/party sections were read (the rest of |save| is zeros).
  bool partial = false;
};

class Manager;

// One GB-Link adapter and the GBA connected to it, found by its USB port (key).
class Link
{
public:
  Link(Manager& owner, std::string key);
  ~Link();
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  void Start();
  void Stop();

  const std::string& Key() const { return m_key; }
  Status GetStatus() const;
  bool GetPadKeys(uint16_t* keys) const;

  void SetControllerEnabled(bool enabled);
  bool IsControllerEnabled() const { return m_controller_enabled; }

  // Which emulated GBA this adapter plays: -2 automatic, -1 none, 0 every GBA, 1-4 that port.
  void SetPortSetting(int setting) { m_port_setting = setting; }
  int PortSetting() const { return m_port_setting; }

  bool WantsMirrorFrames() const { return m_pad_live; }
  void SubmitMirrorFrame(int device, const uint32_t* xbgr, size_t count);

  bool RequestDump(std::function<void(DumpResult)> done, std::function<void(int)> progress,
                   bool team_only);
  void CancelDump();
  bool IsDumpQueued() const;

private:
  struct DumpRequest
  {
    std::function<void(DumpResult)> done;
    std::function<void(int)> progress;
    bool team_only = false;
  };

  void Run();
  void SetStatus(State state, std::string text);
  void LogLine(const std::string& s) const;
  // Waits up to ms; returns early on quit or a queued dump.
  void WaitFor(int ms);

  Manager& m_owner;
  const std::string m_key;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::thread m_thread;
  bool m_quit = false;
  bool m_running = false;
  Status m_status;
  std::unique_ptr<DumpRequest> m_dump;
  std::atomic<bool> m_cancel_dump{false};

  std::atomic<bool> m_controller_enabled{true};
  std::atomic<int> m_port_setting{-2};
  std::atomic<bool> m_pad_live{false};
  std::atomic<int> m_mirror_device{-1};
  ScreenMirror m_mirror;
  std::atomic<uint16_t> m_pad_keys{0};
  mutable std::atomic<uint64_t> m_query_count{0};
  mutable std::atomic<uint64_t> m_query_live{0};
};

struct AdapterInfo
{
  int index = 0;        // 0-based, order of discovery; shown as "GB-Link <index+1>"
  std::string key;      // USB port, e.g. "1-4.2"
  Status status;
  int port_setting = -2;  // what the player chose: -2 automatic, -1 none, 0 all, 1-4
  int port = 0;           // what is in effect: -1 none, 0 every GBA, 1-4 that GBA port
};

// Up to four adapters: finds them by USB port, keeps one Link each, and routes the emulator's
// questions (keys, frames) to the Link playing that emulated GBA.
class Manager
{
public:
  static constexpr int kMaxAdapters = 4;
  static Manager& Get();

  void Start();
  void Stop();

  // One summary for the launcher: the most active adapter's state, and a line per adapter.
  Status GetStatus() const;
  std::vector<AdapterInfo> GetAdapters() const;

  // |device| is the emulated GBA's index (0-3). Live controller state; false when no GBA that
  // plays this device is acting as a controller right now.
  // keys: bit0 A, 1 B, 2 Select, 3 Start, 4 Right, 5 Left, 6 Up, 7 Down, 8 R, 9 L.
  bool GetPadKeys(int device, uint16_t* keys) const;

  // Applies to every adapter (an adapter released by the L+R+Start+Select combo stays released
  // until this is turned on again).
  void SetControllerEnabled(bool enabled);
  bool IsControllerEnabled() const;

  // The adapter at |key| plays GBA port |setting| (-2 automatic, -1 none, 0 every GBA, 1-4).
  void SetPortSetting(const std::string& key, int setting);
  // What a Link with |setting| at slot |index| actually plays right now.
  int EffectivePort(int setting, int index) const;

  // When on, the real GBA that plays GBA port 1 also drives the GameCube pad on port 1.
  void SetMenuNavigation(bool on) { m_menu_nav = on; }
  bool IsMenuNavigation() const { return m_menu_nav; }

  // Show the emulated GBA's screen on the real GBA that plays it (SubmitMirrorFrame, called on
  // the emulation thread with the emulated GBA's index and 240*160 mGBA pixels).
  void SetScreenMirror(bool on) { m_mirror_on = on; }
  bool IsScreenMirror() const { return m_mirror_on; }
  bool WantsMirrorFrames(int device) const;
  void SubmitMirrorFrame(int device, const uint32_t* xbgr, size_t count);

  // Asynchronous, read-only cartridge save dump through the adapter at |key| (empty: the first
  // one). |done| runs on that adapter's thread. |progress| gets 0-100. Returns false if a dump
  // is already queued there or there is no such adapter.
  // team_only: read just the Gen 3 trainer + party sections when the OrreLink agent is running
  // (falls back to a full read for anything else).
  bool RequestDump(const std::string& key, std::function<void(DumpResult)> done,
                   std::function<void(int)> progress = {}, bool team_only = false);
  void CancelDump();
  bool IsDumpQueued() const;

private:
  Manager() = default;
  ~Manager();
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  void Supervise();
  // The Link that plays |device|, preferring one assigned to that exact port over "every GBA".
  Link* LinkFor(int device) const;
  int LinkCount() const;

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::thread m_thread;
  bool m_quit = false;
  bool m_running = false;
  std::vector<std::unique_ptr<Link>> m_owned;      // guarded by m_mutex
  std::array<std::atomic<Link*>, kMaxAdapters> m_links{};  // fixed slots, read without locks
  std::atomic<bool> m_menu_nav{false};
  std::atomic<bool> m_mirror_on{true};
  std::atomic<bool> m_controller_wanted{true};
};
}  // namespace GBLink
