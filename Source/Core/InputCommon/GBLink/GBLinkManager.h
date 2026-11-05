// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Owns the GB-Link USB adapter for the whole Dolphin process. One background thread:
//  * finds the adapter (hot-plug),
//  * when the GBA sits at its multiboot-wait screen, sends it the OrreLink agent,
//  * keeps the GBA in controller mode and publishes its key state (read by GBAPad),
//  * on request, pauses the controller and reads the cartridge save.
// Nothing here ever writes to a cartridge.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

class Manager
{
public:
  static Manager& Get();

  void Start();
  void Stop();

  Status GetStatus() const;

  // Live controller state; false when no GBA is acting as a controller right now.
  // keys: bit0 A, 1 B, 2 Select, 3 Start, 4 Right, 5 Left, 6 Up, 7 Down, 8 R, 9 L.
  bool GetPadKeys(uint16_t* keys) const;

  void SetControllerEnabled(bool enabled);
  bool IsControllerEnabled() const { return m_controller_enabled; }

  // Asynchronous, read-only cartridge save dump. |done| runs on the manager thread.
  // |progress| gets 0-100. Returns false if a dump is already queued.
  // team_only: read just the Gen 3 trainer + party sections when the OrreLink agent is running
  // (falls back to a full read for anything else).
  bool RequestDump(std::function<void(DumpResult)> done, std::function<void(int)> progress = {},
                   bool team_only = false);
  void CancelDump();
  bool IsDumpQueued() const;

private:
  Manager() = default;
  ~Manager();
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  struct DumpRequest
  {
    std::function<void(DumpResult)> done;
    std::function<void(int)> progress;
    bool team_only = false;
  };

  void Run();
  void SetStatus(State state, std::string text);
  // Waits up to ms; returns early on quit or a queued dump.
  void WaitFor(int ms);

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::thread m_thread;
  bool m_quit = false;
  bool m_running = false;
  Status m_status;
  std::unique_ptr<DumpRequest> m_dump;
  std::atomic<bool> m_cancel_dump{false};

  std::atomic<bool> m_controller_enabled{true};
  std::atomic<bool> m_pad_live{false};
  std::atomic<uint16_t> m_pad_keys{0};
};
}  // namespace GBLink
