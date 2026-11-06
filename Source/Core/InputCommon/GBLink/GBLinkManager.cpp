// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputCommon/GBLink/GBLinkManager.h"

#include <algorithm>
#include <map>
#include <chrono>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/CommonPaths.h"
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

void FileLog(const std::string& s)
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "GBLink: {}", s);
  // Also keep a plain file next to Dolphin's other logs; it works with logging switched off.
  static std::mutex file_mutex;
  std::lock_guard lock(file_mutex);
  static File::IOFile file;
  if (!file.IsOpen())
  {
    const std::string path = File::GetUserPath(D_LOGS_IDX) + "GBLink.log";
    File::CreateFullPath(path);
    file.Open(path, "ab");
  }
  if (file.IsOpen())
  {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const std::string line = fmt::format("[{}.{:03}] {}\n", ms / 1000 % 100000, ms % 1000, s);
    file.WriteBytes(line.data(), line.size());
    file.Flush();
  }
}
}  // namespace

Link::Link(Manager& owner, std::string key) : m_owner(owner), m_key(std::move(key))
{
}

Link::~Link()
{
  Stop();
}

void Link::LogLine(const std::string& s) const
{
  FileLog("[" + m_key + "] " + s);
}

void Link::Start()
{
  std::lock_guard lock(m_mutex);
  if (m_running)
    return;
  m_quit = false;
  m_running = true;
  m_status = {State::NoAdapter, "Looking for a GB-Link adapter..."};
  m_thread = std::thread([this] { Run(); });
}

void Link::Stop()
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

Status Link::GetStatus() const
{
  std::lock_guard lock(m_mutex);
  return m_status;
}

void Link::SubmitMirrorFrame(int device, const uint32_t* xbgr, size_t count)
{
  int expected = -1;
  m_mirror_device.compare_exchange_strong(expected, device);
  if (m_mirror_device != device)
    return;
  m_mirror.SubmitFrame(xbgr, count);
}

void Link::SetStatus(State state, std::string text)
{
  std::lock_guard lock(m_mutex);
  if (m_status.state != state || m_status.text != text)
    LogLine(text);
  m_status = {state, std::move(text)};
}

bool Link::GetPadKeys(uint16_t* keys) const
{
  m_query_count++;
  if (!m_pad_live)
    return false;
  m_query_live++;
  *keys = m_pad_keys;
  return true;
}

void Link::SetControllerEnabled(bool enabled)
{
  m_controller_enabled = enabled;
  m_cv.notify_all();
}

bool Link::RequestDump(std::function<void(DumpResult)> done, std::function<void(int)> progress,
                          bool team_only)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_dump || !m_running)
      return false;
    m_dump = std::make_unique<DumpRequest>();
    m_dump->done = std::move(done);
    m_dump->progress = std::move(progress);
    m_dump->team_only = team_only;
    m_cancel_dump = false;
  }
  m_cv.notify_all();
  return true;
}

void Link::CancelDump()
{
  // A dialog that is merely closing after a finished read also lands here; that must not
  // poison whatever the link does next.
  {
    std::lock_guard lock(m_mutex);
    if (!m_dump && !m_dump_active)
      return;
  }
  m_cancel_dump = true;
  m_cv.notify_all();
}

bool Link::IsDumpQueued() const
{
  std::lock_guard lock(m_mutex);
  return m_dump != nullptr;
}

void Link::WaitFor(int ms)
{
  std::unique_lock lock(m_mutex);
  m_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_quit || m_dump; });
}

void Link::Run()
{
  UsbLink usb;
  const LogFn log_fn = [this](const std::string& s) { LogLine(s); };
  usb.SetLog(log_fn);
  std::unique_ptr<GbaCartClient> cart;
  bool agent_loaded = false;
  bool agent_has_pad = false;
  uint64_t stat_polls = 0;
  uint64_t mirror_ok = 0, mirror_failed = 0;
  int mirror_fails = 0;
  std::vector<uint32_t> mirror_packet;
  auto stat_time = Clock::now();
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
    if (!Multiboot(usb, payload, &err, log_fn, 5))
    {
      SetStatus(State::Error, "Could not load the GBA program: " + err);
      return false;
    }
    cart = std::make_unique<GbaCartClient>(usb, log_fn);
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
  auto last_wait_log = Clock::now();
  uint32_t last_wait_reply = 0;
  const auto gba_is_waiting = [&] {
    const uint32_t reply = Spi32(usb, 0x6202);
    last_wait_reply = reply;
    return (reply >> 16) == 0x7202;
  };

  while (!quitting())
  {
    // ---- adapter -----------------------------------------------------------------------
    if (!usb.IsOpen())
    {
      m_pad_live = false;
      std::string err;
      bool opened = false;
      for (const DeviceInfo& d : UsbLink::List())
      {
        if (d.Key() == m_key)
        {
          opened = usb.Open(&err, d.bus, d.address);
          break;
        }
      }
      if (!opened)
      {
        if (auto req = take_dump())
        {
          DumpResult r;
          r.error = "This GB-Link adapter is not available. Plug it in (and close the Cart Doctor browser tab).";
          req->done(std::move(r));
        }
        SetStatus(State::NoAdapter, "GB-Link adapter not available");
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
      // Put the adapter in GB-Link mode with the multiboot timing before polling for the GBA;
      // a freshly plugged adapter reads 0x00000000 until this is done.
      usb.SetMode(MODE_GB_LINK);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      usb.SetTimingConfig(36, 4);
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
        m_dump_active = true;
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
          const bool try_team = req->team_only && agent_has_pad;
          while (true)
          {
            info = try_team ? cart->BeginRanges() : cart->ReadInfo();
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
          const auto report = [&](size_t done, size_t total) {
            const int pct = static_cast<int>(done * 100 / total);
            if (pct != last_pct && req->progress)
            {
              last_pct = pct;
              req->progress(pct);
            }
          };
          const std::string gc = info.code.substr(0, 3);
          const bool gen3 = gc == "AXV" || gc == "AXP" || gc == "BPR" || gc == "BPG" || gc == "BPE";
          bool have_save = false;
          if (try_team)
          {
            if (info.save_size == 131072 && gen3)
            {
              try
              {
                result.save = cart->ReadGen3Team(info.save_size, report);
                result.partial = true;
                have_save = true;
              }
              catch (const GbaCartError& e)
              {
                if (e.code != "desync")
                  throw;
              }
            }
            if (!have_save && info.save_size != 0)
            {
              // Ranged agent but not a Gen 3 team read: fetch the whole save through ranges.
              result.save.clear();
              for (uint32_t off = 0; off < info.save_size; off += 0x4000)
              {
                const uint32_t len = std::min<uint32_t>(0x4000, info.save_size - off);
                const auto part = cart->ReadRange(off, len);
                result.save.insert(result.save.end(), part.begin(), part.end());
                report(off + len, info.save_size);
              }
              result.partial = false;
              have_save = true;
            }
            cart->EndRanges();
            if (!have_save)
              throw GbaCartError("no-save", "This cartridge has no save data.");
          }
          else
          {
            result.save = cart->DumpSave(report);
          }
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
        m_dump_active = false;
        m_cancel_dump = false;
        if (req->done)
          req->done(std::move(result));
        continue;
      }

      // ---- controller ------------------------------------------------------------------
      if (agent_loaded && agent_has_pad && m_controller_enabled)
      {
        if (!pad_active)
        {
          bool image_ok = false;
          size_t image_size = 0;
          const uint8_t* image = StandbyImage(&image_size);
          if (image_size == GbaCartClient::kImageBytes)
          {
            try
            {
              const auto t0 = Clock::now();
              cart->EnterPadModeWithImage(std::vector<uint8_t>(image, image + image_size));
              image_ok = true;
              LogLine(fmt::format(
                  "picture sent in {} ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
            }
            catch (const GbaCartError& e)
            {
              LogLine(std::string("could not send the picture: ") + e.what());
              if (e.code == "link")
                throw;
              // The GBA may be waiting inside the picture transfer: let it time out.
              if (!cart->Ping(3000))
                throw;
            }
          }
          if (!image_ok)
            cart->EnterPadMode();
          pad_active = true;
          m_mirror.Reset();
          m_mirror_device = -1;
          mirror_fails = 0;
          bad_polls = 0;
          SetStatus(State::ControllerActive, "Your GBA is acting as a controller");
        }
        uint16_t keys = 0;
        uint32_t raw = 0;
        stat_polls++;
        if (m_owner.IsScreenMirror() && mirror_fails < 5 && m_mirror.NextPacket(&mirror_packet, 12))
        {
          uint16_t packet_keys = m_pad_keys;
          if (cart->SendTiles(mirror_packet, &packet_keys))
          {
            m_mirror.PacketAcked();
            mirror_fails = 0;
            mirror_ok++;
            m_pad_keys = packet_keys;
          }
          else
          {
            m_mirror.PacketFailed();
            mirror_failed++;
            if (++mirror_fails >= 5)
              LogLine("screen mirror switched off: the GBA did not acknowledge 5 packets in a row "
                      "(old agent?)");
          }
        }
        if (cart->PollPad(&keys, &raw))
        {
          bad_polls = 0;
          if (keys != m_pad_keys)
            LogLine(fmt::format("GBA keys 0x{:03x}", keys));
          m_pad_keys = keys;
          m_pad_live = true;
        }
        else if (++bad_polls >= kPadBadPollsBeforeCheck)
        {
          LogLine(fmt::format("{} bad pad polls in a row, last reply 0x{:08x}", bad_polls, raw));
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
        if (Clock::now() - stat_time >= std::chrono::seconds(5))
        {
          LogLine(fmt::format("controller: {} polls, Dolphin asked for keys {} times ({} live), "
                              "live={} keys=0x{:03x}",
                              stat_polls, m_query_count.exchange(0), m_query_live.exchange(0),
                              m_pad_live.load(), m_pad_keys.load()));
          LogLine(fmt::format("mirror: {} packets acknowledged, {} failed (device {})", mirror_ok,
                              mirror_failed, m_mirror_device.load()));
          mirror_ok = mirror_failed = 0;
          stat_polls = 0;
          stat_time = Clock::now();
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
          {
            // Say now and then what the adapter sees, so a GBA that never shows up can be told
            // apart from a cable or power problem (a GBA waiting for a program reads 0x7202xxxx).
            if (Clock::now() - last_wait_log > std::chrono::seconds(15))
            {
              last_wait_log = Clock::now();
              LogLine(fmt::format("still waiting for a GBA at the multiboot screen: the link reads "
                                  "0x{:08x} (a waiting GBA reads 0x7202....)",
                                  last_wait_reply));
            }
            SetStatus(State::WaitingForGba,
                      "GB-Link connected. Power on your GBA holding START + SELECT "
                      "(no cartridge needed) to use it as a controller.");
          }
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

namespace GBLink
{
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
  for (auto& l : m_owned)
    l->Start();
  m_thread = std::thread([this] { Supervise(); });
}

void Manager::Stop()
{
  {
    std::lock_guard lock(m_mutex);
    if (!m_running)
      return;
    m_quit = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  std::vector<Link*> links;
  {
    std::lock_guard lock(m_mutex);
    m_running = false;
    for (auto& l : m_owned)
      links.push_back(l.get());
  }
  // The Links stay allocated (the emulator may still ask them for keys); they just stop.
  for (Link* l : links)
    l->Stop();
}

void Manager::Supervise()
{
  std::map<std::string, int> missing;  // seconds an adapter's USB port has been empty
  while (true)
  {
    std::vector<DeviceInfo> found = UsbLink::List();
    std::sort(found.begin(), found.end(),
              [](const DeviceInfo& a, const DeviceInfo& b) { return a.Key() < b.Key(); });
    std::vector<Link*> retire;
    {
      std::lock_guard lock(m_mutex);
      if (m_quit)
        return;
      std::vector<Link*> active;
      for (auto& slot : m_links)
      {
        if (Link* l = slot.load())
          active.push_back(l);
      }
      // An adapter that was unplugged (or moved to another port) gives its slot up after a few
      // seconds, so a moved adapter does not leave a ghost row behind or use up one of the four.
      for (Link* l : active)
      {
        const bool present = std::any_of(found.begin(), found.end(),
                                         [&](const DeviceInfo& d) { return d.Key() == l->Key(); });
        if (present)
          missing.erase(l->Key());
        else if (++missing[l->Key()] >= 5)
          retire.push_back(l);
      }
    }
    for (Link* l : retire)
      l->Stop();
    {
      std::lock_guard lock(m_mutex);
      if (m_quit)
        return;
      std::vector<Link*> active;
      for (auto& slot : m_links)
      {
        Link* l = slot.load();
        if (l && std::find(retire.begin(), retire.end(), l) == retire.end())
          active.push_back(l);
        else if (l)
          missing.erase(l->Key());
      }
      for (const DeviceInfo& d : found)
      {
        const std::string key = d.Key();
        const bool known = std::any_of(active.begin(), active.end(),
                                       [&](Link* l) { return l->Key() == key; });
        if (known || active.size() >= size_t(kMaxAdapters))
          continue;
        auto link = std::make_unique<Link>(*this, key);
        link->SetControllerEnabled(m_controller_wanted);
        link->Start();
        active.push_back(link.get());
        m_owned.push_back(std::move(link));  // retired Links stay allocated: the emulator may
                                             // still hold a pointer to one for a moment
      }
      for (size_t i = 0; i < m_links.size(); i++)
        m_links[i] = i < active.size() ? active[i] : nullptr;
    }
    std::unique_lock lock(m_mutex);
    m_cv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return m_quit; });
    if (m_quit)
      return;
  }
}

int Manager::LinkCount() const
{
  int n = 0;
  for (const auto& slot : m_links)
    n += slot.load() != nullptr;
  return n;
}

int Manager::EffectivePort(int setting, int index) const
{
  if (setting != -2)
    return setting;
  return LinkCount() <= 1 ? 0 : index + 1;
}

Link* Manager::LinkFor(int device) const
{
  Link* all = nullptr;
  for (int i = 0; i < kMaxAdapters; i++)
  {
    Link* l = m_links[i].load();
    if (!l)
      continue;
    const int port = EffectivePort(l->PortSetting(), i);
    if (port == device + 1)
      return l;
    if (port == 0 && !all)
      all = l;
  }
  return all;
}

std::vector<AdapterInfo> Manager::GetAdapters() const
{
  std::vector<AdapterInfo> out;
  for (int i = 0; i < kMaxAdapters; i++)
  {
    Link* l = m_links[i].load();
    if (!l)
      continue;
    AdapterInfo info;
    info.index = i;
    info.key = l->Key();
    info.status = l->GetStatus();
    info.port_setting = l->PortSetting();
    info.port = EffectivePort(info.port_setting, i);
    out.push_back(std::move(info));
  }
  return out;
}

Status Manager::GetStatus() const
{
  const std::vector<AdapterInfo> adapters = GetAdapters();
  if (adapters.empty())
    return {State::NoAdapter, "No GB-Link adapter found"};
  if (adapters.size() == 1)
    return adapters[0].status;
  const auto rank = [](State s) {
    switch (s)
    {
    case State::ReadingCart:
      return 7;
    case State::ControllerActive:
      return 6;
    case State::LoadingAgent:
      return 5;
    case State::AgentIdle:
      return 4;
    case State::WaitingForGba:
      return 3;
    case State::Error:
      return 2;
    case State::NoAdapter:
      return 1;
    default:
      return 0;
    }
  };
  Status best = adapters[0].status;
  std::string text;
  for (const AdapterInfo& a : adapters)
  {
    if (rank(a.status.state) > rank(best.state))
      best.state = a.status.state;
    text += (text.empty() ? "" : "\n") + fmt::format("GB-Link {}: {}", a.index + 1, a.status.text);
  }
  best.text = std::move(text);
  return best;
}

bool Manager::GetPadKeys(int device, uint16_t* keys) const
{
  Link* l = LinkFor(device);
  return l && l->GetPadKeys(keys);
}

void Manager::SetControllerEnabled(bool enabled)
{
  m_controller_wanted = enabled;
  for (auto& slot : m_links)
  {
    if (Link* l = slot.load())
      l->SetControllerEnabled(enabled);
  }
}

bool Manager::IsControllerEnabled() const
{
  bool any = false, have = false;
  for (const auto& slot : m_links)
  {
    if (Link* l = slot.load())
    {
      have = true;
      any = any || l->IsControllerEnabled();
    }
  }
  return have ? any : m_controller_wanted.load();
}

void Manager::SetPortSetting(const std::string& key, int setting)
{
  for (auto& slot : m_links)
  {
    Link* l = slot.load();
    if (l && l->Key() == key)
      l->SetPortSetting(setting);
  }
}

bool Manager::WantsMirrorFrames(int device) const
{
  if (!m_mirror_on)
    return false;
  Link* l = LinkFor(device);
  return l && l->WantsMirrorFrames();
}

void Manager::SubmitMirrorFrame(int device, const uint32_t* xbgr, size_t count)
{
  if (Link* l = LinkFor(device))
    l->SubmitMirrorFrame(device, xbgr, count);
}

bool Manager::RequestDump(const std::string& key, std::function<void(DumpResult)> done,
                          std::function<void(int)> progress, bool team_only)
{
  for (auto& slot : m_links)
  {
    Link* l = slot.load();
    if (l && (key.empty() || l->Key() == key))
      return l->RequestDump(std::move(done), std::move(progress), team_only);
  }
  return false;
}

void Manager::CancelDump()
{
  for (auto& slot : m_links)
  {
    if (Link* l = slot.load())
      l->CancelDump();
  }
}

bool Manager::IsDumpQueued() const
{
  for (const auto& slot : m_links)
  {
    Link* l = slot.load();
    if (l && l->IsDumpQueued())
      return true;
  }
  return false;
}
}  // namespace GBLink
