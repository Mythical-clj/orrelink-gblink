// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/JoyBusLagModel.h"
#include "Core/HW/SI/SI_Device.h"

namespace SerialInterface
{
// True when a GB-Link adapter with the JOY Bus firmware is plugged in, so "Real GBA" can be the
// default without leaving a machine that has none without GBAs.
bool GBLinkJoybusAvailable();

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

  // Netplay (and the lag test): what the game hears comes from the lag model. Model index i is
  // command number i + 1 (m_seq) on every machine.
  static constexpr u64 kLagMs = 100;
  std::unique_ptr<JoyBusLagModel> m_model;
  u32 m_model_index = 0;
  std::vector<JoyBusLagModel::Reply> m_real;  // the owner's: the GBA's replies so far
  std::array<u32, 3> m_stat_how{};

  // For GBLink.log, every five seconds: how much the game talks to the GBA and what it costs.
  std::chrono::steady_clock::time_point m_stat_time;
  u32 m_stat_status = 0, m_stat_read = 0, m_stat_write = 0, m_stat_none = 0;
  double m_stat_usb_ms = 0, m_stat_usb_max = 0;
  double m_stat_net_ms = 0, m_stat_net_max = 0;
  u32 m_stat_net_waits = 0;
  std::array<u8, 5> m_last_logged{};
  std::array<u8, 5> m_tx{};
  u32 m_trace_left = 60;  // the first commands are logged one by one (all go to JoyTrace.txt)
};
}  // namespace SerialInterface
