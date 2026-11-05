// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputCommon/GBLink/Manager.h"

#include <chrono>
#include <utility>

#include "Common/Logging/Log.h"
#include "InputCommon/GBLink/GbaCartClient.h"
#include "InputCommon/GBLink/Multiboot.h"
#include "InputCommon/GBLink/Payloads.h"
#include "InputCommon/GBLink/Spi32.h"
#include "InputCommon/GBLink/UsbLink.h"

namespace GBLink
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int kDumpWaitForGbaSeconds = 90;
constexpr int kDumpWaitForCartSeconds = 60;
constexpr int kPadBadPollsBeforeCheck = 20;

void LogLine(const std::string& s)
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "GBLink: {}", s);
}
}  // namespace

Manager& Manager::Get()
{
  static Manager instance;
  return instance;
}

Manager::~Manager()
{
  Stop();
}

void Manager::Start()
{
  std::lock_guard lock(m_mutex);
  if (m_running)
    return;
  m_quit = false;
  m_running = true;
  m_status = {State::NoAdapter, "Looking for a GB-Link adapter..."};
  m_thread = std::thread([this] { Run(); });
}

void Manager::Stop()
{
  {
    std::lock_guard lock(m_mutex);
    if (!m_running)
      return;
    m_quit = true;
  }
  m_cancel_dump = true;
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  std::lock_guard lock(m_mutex);
  m_running = false;
  m_status = {State::Stopped, "GB-Link support stopped"};
  m_pad_live = false;
}

Status Manager::GetStatus() const
{
  std::lock_guard lock(m_mutex);
  return m_status;
}

void Manager::SetStatus(State state, std::string text)
{
  std::lock_guard lock(m_mutex);
  if (m_status.state != state || m_status.text != text)
    LogLine(text);
  m_status = {state, std::move(text)};
}

bool Manager::GetPadKeys(uint16_t* keys) const
{
  if (!m_pad_live)
    return false;
  *keys = m_pad_keys;
  return true;
}

void Manager::SetControllerEnabled(bool enabled)
{
  m_controller_enabled = enabled;
  m_cv.notify_all();
}

bool Manager::RequestDump(std::function<void(DumpResult)> done, std::function<void(int)> progress)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_dump || !m_running)
      return false;
    m_dump = std::make_unique<DumpRequest>();
    m_dump->done = std::move(done);
    m_dump->progress = std::move(progress);
    m_cancel_dump = false;
  }
  m_cv.notify_all();
  return true;
}

void Manager::CancelDump()
{
  m_cancel_dump = true;
  m_cv.notify_all();
}

bool Manager::IsDumpQueued() const
{
  std::lock_guard lock(m_mutex);
  return m_dump != nullptr;
}

void Manager::WaitFor(int ms)
{
  std::unique_lock lock(m_mutex);
  m_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_quit || m_dump; });
}

void Manager::Run()
{
  UsbLink usb;
  usb.SetLog(LogLine);
  std::unique_ptr<GbaCartClient> cart;
  bool agent_loaded = false;
  bool agent_has_pad = false;
  bool pad_active = false;
  int bad_polls = 0;

  const auto quitting = [this] {
    std::lock_guard lock(m_mutex);
    return m_quit;
  };
  const auto take_dump = [this]() -> std::unique_ptr<DumpRequest> {
    std::lock_guard lock(m_mutex);
    return std::move(m_dump);
  };
  const auto dump_pending = [this] {
    std::lock_guard lock(m_mutex);
    return m_dump != nullptr;
  };

  const auto drop_adapter = [&] {
    m_pad_live = false;
    pad_active = false;
    agent_loaded = false;
    cart.reset();
    usb.Close();
  };

  // Sends the OrreLink agent (or the stock dumper as a fallback) to a GBA that is waiting.
  const auto load_agent = [&]() -> bool {
    size_t size = 0;
    const uint8_t* data = AgentPayload(&size);
    agent_has_pad = size > 0;
    if (!agent_has_pad)
      data = DumperPayload(&size);
    const std::vector<uint8_t> payload(data, data + size);
    SetStatus(State::LoadingAgent, "Sending the OrreLink program to your GBA...");
    std::string err;
    if (!Multiboot(usb, payload, &err, LogLine, 5))
    {
      SetStatus(State::Error, "Could not load the GBA program: " + err);
      return false;
    }
    cart = std::make_unique<GbaCartClient>(usb, LogLine);
    cart->SetExternalCancel(&m_cancel_dump);
    if (!cart->Ping(15000))
    {
      SetStatus(State::Error, "The GBA program did not answer after loading.");
      cart.reset();
      return false;
    }
    agent_loaded = true;
    pad_active = false;
    bad_polls = 0;
    return true;
  };

  // Polls for the GBA's multiboot handshake. True if it is waiting for a program.
  const auto gba_is_waiting = [&] { return (Spi32(usb, 0x6202) >> 16) == 0x7202; };

  while (!quitting())
  {
    // ---- adapter -----------------------------------------------------------------------
    if (!usb.IsOpen())
    {
      m_pad_live = false;
      std::string err;
      if (!usb.Open(&err))
      {
        if (auto req = take_dump())
        {
          DumpResult r;
          r.error = "No GB-Link adapter found. Plug it in (and close the Cart Doctor browser tab).";
          req->done(std::move(r));
        }
        SetStatus(State::NoAdapter, "No GB-Link adapter found");
        WaitFor(1000);
        continue;
      }
      if (!usb.IsNewFirmware())
      {
        SetStatus(State::Error, "This adapter has old firmware. Update it at launcher.gblink.io.");
        usb.Close();
        WaitFor(3000);
        continue;
      }
      usb.DrainData();
      usb.SetVoltage3V3();
      usb.DrainData();
      agent_loaded = false;
      pad_active = false;
      SetStatus(State::WaitingForGba, "GB-Link connected. Waiting for your GBA...");
    }

    try
    {
      // ---- cartridge dump request ------------------------------------------------------
      if (dump_pending())
      {
        auto req = take_dump();
        DumpResult result;
        try
        {
          if (!agent_loaded)
          {
            SetStatus(State::WaitingForGba,
                      "Power on your GBA now while holding START + SELECT (cartridge can stay in).");
            const auto deadline = Clock::now() + std::chrono::seconds(kDumpWaitForGbaSeconds);
            bool ok = false;
            while (!quitting() && !m_cancel_dump && Clock::now() < deadline)
            {
              if (gba_is_waiting())
              {
                ok = load_agent();
                break;
              }
              WaitFor(30);
            }
            if (!ok)
            {
              if (m_cancel_dump)
                throw GbaCartError("cancelled", "Cancelled");
              throw GbaCartError("timeout",
                                 "The GBA never showed up. Power it on holding START + SELECT, "
                                 "check the cable (small plug in the adapter), then try again.");
            }
          }
          else if (pad_active)
          {
            m_pad_live = false;
            cart->LeavePadMode();
            pad_active = false;
          }

          SetStatus(State::ReadingCart, "Reading your cartridge...");
          CartInfo info;
          const auto cart_deadline = Clock::now() + std::chrono::seconds(kDumpWaitForCartSeconds);
          while (true)
          {
            info = cart->ReadInfo();
            if (!info.no_cart)
              break;
            SetStatus(State::ReadingCart,
                      "No cartridge detected. Insert your cartridge into the GBA (leave it on).");
            if (quitting() || m_cancel_dump || Clock::now() > cart_deadline)
              throw GbaCartError("no-cart", "No cartridge was detected in the GBA.");
            WaitFor(2000);
          }
          result.title = info.title;
          result.code = info.code;
          SetStatus(State::ReadingCart, "Reading the save from " + info.title + "...");
          int last_pct = -1;
          result.save = cart->DumpSave([&](size_t done, size_t total) {
            const int pct = static_cast<int>(done * 100 / total);
            if (pct != last_pct && req->progress)
            {
              last_pct = pct;
              req->progress(pct);
            }
          });
          result.ok = true;
        }
        catch (const GbaCartError& e)
        {
          result.ok = false;
          result.error = e.what();
          if (e.code == "link" || e.code == "desync" || e.code == "timeout")
            agent_loaded = false;  // state unknown: ask for a fresh start
        }
        if (usb.Lost())
          drop_adapter();
        else
          SetStatus(State::AgentIdle, result.ok ? "Cartridge read." : result.error);
        if (req->done)
          req->done(std::move(result));
        continue;
      }

      // ---- controller ------------------------------------------------------------------
      if (agent_loaded && agent_has_pad && m_controller_enabled)
      {
        if (!pad_active)
        {
          cart->EnterPadMode();
          pad_active = true;
          bad_polls = 0;
          SetStatus(State::ControllerActive, "Your GBA is acting as a controller");
        }
        uint16_t keys = 0;
        if (cart->PollPad(&keys))
        {
          bad_polls = 0;
          m_pad_keys = keys;
          m_pad_live = true;
        }
        else if (++bad_polls >= kPadBadPollsBeforeCheck)
        {
          m_pad_live = false;
          m_pad_keys = 0;
          pad_active = false;
          if (cart->Ping(500))
          {
            // The GBA answers as an idle agent: the player pressed L+R+Start+Select.
            m_controller_enabled = false;
            SetStatus(State::AgentIdle, "GBA controller paused (L+R+Start+Select).");
          }
          else
          {
            agent_loaded = false;
            SetStatus(State::WaitingForGba, "Lost the GBA. Power it on holding START + SELECT.");
          }
        }
        if (usb.Lost())
          drop_adapter();
        else
          WaitFor(1);
        continue;
      }

      // ---- waiting for a GBA -----------------------------------------------------------
      if (!agent_loaded)
      {
        if (m_controller_enabled)
        {
          if (gba_is_waiting())
            load_agent();
          else
            SetStatus(State::WaitingForGba,
                      "GB-Link connected. Power on your GBA holding START + SELECT "
                      "(no cartridge needed) to use it as a controller.");
          if (usb.Lost())
            drop_adapter();
          else
            WaitFor(50);
        }
        else
        {
          SetStatus(State::WaitingForGba, "GB-Link connected (GBA controller is off).");
          WaitFor(500);
        }
        continue;
      }

      // Agent running but nothing to do (controller off): idle without touching the link.
      SetStatus(State::AgentIdle, "GBA program is running. Controller is off.");
      WaitFor(250);
    }
    catch (const GbaCartError& e)
    {
      m_pad_live = false;
      pad_active = false;
      agent_loaded = false;
      SetStatus(State::WaitingForGba, std::string("GBA link problem: ") + e.what());
      if (usb.Lost())
        drop_adapter();
      WaitFor(500);
    }
  }

  m_pad_live = false;
  if (usb.IsOpen() && cart && pad_active)
  {
    try
    {
      cart->LeavePadMode();
    }
    catch (...)
    {
    }
  }
  usb.Close();
}
}  // namespace GBLink
