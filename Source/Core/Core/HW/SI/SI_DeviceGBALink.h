// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"

namespace SerialInterface
{
// A real GBA on a GB-Link adapter in JOY Bus mode (InputCommon/GBLink): every JOY Bus command the
// game sends goes to the GBA, which runs its own cartridge, and the GBA's reply goes back to the
// game. In netplay the machine that owns the port talks to its GBA and sends each reply to the
// room; the other machines wait for it, so every machine gives the game the same bytes.
class CSIDevice_GBALink final : public ISIDevice
{
public:
  CSIDevice_GBALink(Core::System& system, SIDevices device, int device_number);
  ~CSIDevice_GBALink() override;

  int RunBuffer(u8* buffer, int request_length) override;
  int TransferInterval() override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;

private:
  enum class NextAction
  {
    SendCommand,
    WaitTransferTime,
    ReceiveResponse
  };

  void LogStats();

  NextAction m_next_action = NextAction::SendCommand;
  EBufferCommands m_last_cmd = EBufferCommands::CMD_STATUS;
  u64 m_timestamp_sent = 0;
  u32 m_seq = 0;
  int m_rx_len = 0;
  int m_owner = 0;        // of the command in flight: 0 nobody, 1 this machine, 2 another machine
  int m_link_device = 0;  // which adapter (the local pad number, as the GBA's keys use)
  bool m_started = false;

  // For GBLink.log, every five seconds: how much the game talks to the GBA and what it costs.
  std::chrono::steady_clock::time_point m_stat_time;
  u32 m_stat_status = 0, m_stat_read = 0, m_stat_write = 0, m_stat_none = 0;
  double m_stat_usb_ms = 0, m_stat_usb_max = 0;
  double m_stat_net_ms = 0, m_stat_net_max = 0;
  u32 m_stat_net_waits = 0;
  std::array<u8, 5> m_last_logged{};
  u32 m_trace_left = 400;  // the first commands are logged one by one
};
}  // namespace SerialInterface
