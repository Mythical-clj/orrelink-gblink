// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/SI/SI_DeviceGBALink.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <fstream>
#include <mutex>

#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "Core/HW/SystemTimers.h"

#if defined(__LIBUSB__) || defined(ANDROID)
#define ORRELINK_JOYBUS 1
#include "InputCommon/GBLink/GBLinkManager.h"
#endif

namespace SerialInterface
{
namespace
{
void JoyLog(const std::string& line)
{
#ifdef ORRELINK_JOYBUS
  GBLink::Manager::Get().Log(line);
#else
  (void)line;
#endif
}

// Logs/JoyTrace.txt: every JOY Bus command of the session and its reply, with the emulated time,
// so the game's protocol can be studied (and a netplay model checked against it) offline. One
// line per command: port, emulated ticks, command, the word written or "-", the reply or "--";
// a status asked again with the same reply only counts up ("xN" on the line that ends the run).
class JoyTrace
{
public:
  static JoyTrace& Get()
  {
    static JoyTrace trace;
    return trace;
  }

  void Open(u64 ticks_per_second)
  {
    std::lock_guard lock(m_mutex);
    if (m_users++ > 0)
      return;
    m_file.close();
    m_file.clear();
    File::OpenFStream(m_file, File::GetUserPath(D_LOGS_IDX) + "JoyTrace.txt",
                      std::ios::out | std::ios::trunc);
    m_bytes = 0;
    m_repeat = {};
    Write(fmt::format("# port ticks cmd word reply  ({} ticks per second)\n", ticks_per_second));
  }

  void Close()
  {
    std::lock_guard lock(m_mutex);
    if (m_users == 0 || --m_users > 0)
      return;
    for (int port = 0; port < 4; port++)
      FlushRepeat(port);
    m_file.close();
  }

  void Add(int port, u64 ticks, u8 cmd, const u8* tx, const u8* rx, int got)
  {
    std::lock_guard lock(m_mutex);
    if (!m_file.is_open() || m_bytes > kMaxBytes || port < 0 || port >= 4)
      return;
    std::string reply = "--";
    if (got > 0)
    {
      reply.clear();
      for (int i = 0; i < got; i++)
        reply += fmt::format("{:02x}", rx[i]);
    }
    Repeat& rep = m_repeat[port];
    if (cmd == 0x00 && rep.count > 0 && rep.reply == reply)
    {
      rep.count++;
      rep.last_ticks = ticks;
      return;
    }
    FlushRepeat(port);
    if (cmd == 0x00)
    {
      rep.count = 1;
      rep.reply = reply;
      rep.first_ticks = rep.last_ticks = ticks;
      return;
    }
    Write(fmt::format("{} {} {:02x} {} {}\n", port + 1, ticks, cmd,
                      cmd == 0x15 ? fmt::format("{:02x}{:02x}{:02x}{:02x}", tx[1], tx[2], tx[3],
                                                tx[4]) :
                                    std::string("-"),
                      reply));
  }

private:
  static constexpr u64 kMaxBytes = 48 * 1024 * 1024;
  struct Repeat
  {
    u32 count = 0;
    u64 first_ticks = 0, last_ticks = 0;
    std::string reply;
  };

  void FlushRepeat(int port)
  {
    Repeat& rep = m_repeat[port];
    if (rep.count == 0)
      return;
    Write(rep.count == 1 ?
              fmt::format("{} {} 00 - {}\n", port + 1, rep.first_ticks, rep.reply) :
              fmt::format("{} {} 00 - {} x{} {}\n", port + 1, rep.first_ticks, rep.reply,
                          rep.count, rep.last_ticks));
    rep.count = 0;
  }

  void Write(const std::string& text)
  {
    if (!m_file.is_open())
      return;
    m_file << text;
    m_bytes += text.size();
  }

  std::mutex m_mutex;
  std::ofstream m_file;
  u64 m_bytes = 0;
  int m_users = 0;
  std::array<Repeat, 4> m_repeat;
};

}  // namespace

bool GBLinkJoybusAvailable()
{
#ifdef ORRELINK_JOYBUS
  return GBLink::Manager::Get().JoyReadyCount() > 0;
#else
  return false;
#endif
}

CSIDevice_GBALink::CSIDevice_GBALink(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number), m_stat_time(std::chrono::steady_clock::now())
{
#ifdef ORRELINK_JOYBUS
  // The adapters have to be GameCube ports while this device exists, whatever the launcher says
  // (a netplay guest gets this device from the host's setting).
  GBLink::Manager::Get().SetJoybus(true);
#endif
  JoyLog(fmt::format("joybus port {}: real GBA device created{}", m_device_number + 1,
                     NetPlay::IsNetPlayRunning() ? " (netplay)" : ""));
  JoyTrace::Get().Open(m_system.GetSystemTimers().GetTicksPerSecond());
}

CSIDevice_GBALink::~CSIDevice_GBALink()
{
  LogStats();
  JoyTrace::Get().Close();
#ifdef ANDROID
  GBLink::Manager::Get().SetJoybus(false);
#elif defined(ORRELINK_JOYBUS)
  GBLink::Manager::Get().SetJoybus(Config::Get(Config::MAIN_GBLINK_JOYBUS));
#endif
}

int CSIDevice_GBALink::RunBuffer(u8* buffer, int request_length)
{
  switch (m_next_action)
  {
  case NextAction::SendCommand:
  {
    m_last_cmd = static_cast<EBufferCommands>(buffer[0]);
    int tx_len = 1;
    switch (m_last_cmd)
    {
    case EBufferCommands::CMD_RESET:
    case EBufferCommands::CMD_STATUS:
      m_rx_len = 3;
      break;
    case EBufferCommands::CMD_READ_GBA:
      m_rx_len = 5;
      break;
    case EBufferCommands::CMD_WRITE_GBA:
      tx_len = 5;
      m_rx_len = 1;
      break;
    default:
      return -1;  // not a GBA command: a GBA says nothing
    }

    std::copy(buffer, buffer + 5, m_tx.begin());
    m_seq++;
    m_owner = NetPlay::JoyOwner(m_device_number);
    m_link_device = NetPlay::GbaInputPadFor(m_device_number);
    m_started = false;
#ifdef ORRELINK_JOYBUS
    if (m_owner == 1 && m_link_device >= 0)
      m_started = GBLink::Manager::Get().JoyBegin(m_link_device, buffer, tx_len, m_rx_len);
#else
    (void)tx_len;
#endif
    if (m_trace_left > 0 && m_last_cmd != EBufferCommands::CMD_STATUS)
    {
      JoyLog(fmt::format("joybus port {} #{}: > {:02x} {:02x}{:02x}{:02x}{:02x}",
                         m_device_number + 1, m_seq, buffer[0], buffer[1], buffer[2], buffer[3],
                         buffer[4]));
    }
    m_timestamp_sent = m_system.GetCoreTiming().GetTicks();
    m_next_action = NextAction::WaitTransferTime;
    return 0;
  }

  case NextAction::WaitTransferTime:
  {
    const int elapsed_time =
        static_cast<int>(m_system.GetCoreTiming().GetTicks() - m_timestamp_sent);
    // Tell SI to ask again after TransferInterval() cycles
    if (SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_last_cmd) > elapsed_time)
      return 0;
    m_next_action = NextAction::ReceiveResponse;
    [[fallthrough]];
  }

  case NextAction::ReceiveResponse:
  {
    m_next_action = NextAction::SendCommand;
    u8 rx[5] = {};
    int got = -1;
    if (m_owner == 1)
    {
#ifdef ORRELINK_JOYBUS
      if (m_started)
      {
        double usb_ms = 0;
        got = GBLink::Manager::Get().JoyEnd(m_link_device, rx, &usb_ms);
        m_stat_usb_ms += usb_ms;
        m_stat_usb_max = std::max(m_stat_usb_max, usb_ms);
      }
#endif
      if (got != m_rx_len)
        got = -1;  // a short reply is no reply, on every machine
      if (NetPlay::IsNetPlayRunning())
        NetPlay::SendJoyReply(m_device_number, m_seq, got, rx);
    }
    else if (m_owner == 2)
    {
      const auto t0 = std::chrono::steady_clock::now();
      int len = -1;
      if (NetPlay::WaitJoyReply(m_device_number, m_seq, &len, rx))
        got = len;
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      m_stat_net_ms += ms;
      m_stat_net_max = std::max(m_stat_net_max, ms);
      m_stat_net_waits++;
      if (got != m_rx_len)
        got = -1;
    }

    switch (m_last_cmd)
    {
    case EBufferCommands::CMD_READ_GBA:
      m_stat_read++;
      break;
    case EBufferCommands::CMD_WRITE_GBA:
      m_stat_write++;
      break;
    default:
      m_stat_status++;
      break;
    }
    if (got <= 0)
      m_stat_none++;
    JoyTrace::Get().Add(m_device_number, m_timestamp_sent, m_tx[0], m_tx.data(), rx, got);

    // The first commands one by one (a status only when its reply changes), then the totals.
    if (m_trace_left > 0)
    {
      std::array<u8, 5> now{};
      if (got > 0)
        std::copy(rx, rx + got, now.begin());
      const bool is_status = m_last_cmd == EBufferCommands::CMD_STATUS;
      if (!is_status || now != m_last_logged)
      {
        m_trace_left--;
        if (is_status)
          m_last_logged = now;
        JoyLog(got > 0 ?
                   fmt::format("joybus port {} #{}: {:02x} < {:02x}{:02x}{:02x}{:02x}{:02x} ({})",
                               m_device_number + 1, m_seq, static_cast<u8>(m_last_cmd), rx[0],
                               rx[1], rx[2], rx[3], rx[4], got) :
                   fmt::format("joybus port {} #{}: {:02x} < no reply", m_device_number + 1,
                               m_seq, static_cast<u8>(m_last_cmd)));
      }
    }
    if (std::chrono::steady_clock::now() - m_stat_time >= std::chrono::seconds(5))
      LogStats();

    if (got <= 0)
      return -1;
    std::memcpy(buffer, rx, got);
    return got;
  }
  }

  return 0;
}

void CSIDevice_GBALink::LogStats()
{
  const u32 total = m_stat_status + m_stat_read + m_stat_write;
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - m_stat_time).count();
  if (total > 0 && seconds > 0)
  {
    static const char* const owners[] = {"nobody's", "mine", "remote"};
    std::string line = fmt::format(
        "joybus port {} ({}): {:.0f} commands/s (status {}, read {}, write {}), {} unanswered",
        m_device_number + 1, owners[std::clamp(m_owner, 0, 2)], total / seconds, m_stat_status,
        m_stat_read, m_stat_write, m_stat_none);
    if (m_stat_net_waits > 0)
    {
      line += fmt::format(", waited for the room {:.1f} ms each (max {:.1f}), {:.0f} ms per second",
                          m_stat_net_ms / m_stat_net_waits, m_stat_net_max,
                          m_stat_net_ms / seconds);
    }
    else if (m_owner == 1)
    {
      line += fmt::format(", adapter {:.2f} ms each (max {:.2f}), {:.0f} ms per second",
                          m_stat_usb_ms / total, m_stat_usb_max, m_stat_usb_ms / seconds);
    }
    JoyLog(line);
  }
  m_stat_status = m_stat_read = m_stat_write = m_stat_none = m_stat_net_waits = 0;
  m_stat_usb_ms = m_stat_usb_max = m_stat_net_ms = m_stat_net_max = 0;
  m_stat_time = std::chrono::steady_clock::now();
}

int CSIDevice_GBALink::TransferInterval()
{
  return SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_last_cmd);
}

DataResponse CSIDevice_GBALink::GetData(u32& hi, u32& low)
{
  return DataResponse::NoData;
}

void CSIDevice_GBALink::SendCommand(u32 command, u8 poll)
{
}
}  // namespace SerialInterface
