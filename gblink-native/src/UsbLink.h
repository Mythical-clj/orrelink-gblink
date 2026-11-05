// Native USB transport for the GB-Link adapter (RP2040, Zephyr firmware).
// Mirrors the WebUSB client in GB-Link/gb-cart-doctor-web (usb_connection.js):
//   new firmware (VID 0x2FE3): two bulk OUT + two bulk IN endpoints on the
//   vendor interface (class 0xFF): [0] = command channel, [1] = data channel.
//   old firmware (VID 0xCAFE / 0x239A): a single pair, CDC-style control 0x22.
// Licensed GPL-3.0-or-later together with the Cart Doctor protocol port.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace GBLink
{
using LogFn = std::function<void(const std::string&)>;

struct DeviceInfo
{
  uint16_t vid = 0, pid = 0, bcd = 0;
  uint8_t bus = 0, address = 0;
  std::string product, manufacturer, serial;
  bool IsNewFirmware() const { return vid == 0x2FE3; }
};

// Command bytes on the command channel (firmware README, "USB Command Protocol").
enum Cmd : uint8_t
{
  CMD_SET_MODE = 0x00,
  CMD_CANCEL = 0x01,
  CMD_GET_FIRMWARE_INFO = 0x0F,
  CMD_SET_TIMING = 0x30,
  CMD_VOLTAGE_3V3 = 0x40,
  CMD_VOLTAGE_5V = 0x41,
  CMD_GET_CABLE_TYPE = 0x4A,
};
enum Mode : uint8_t
{
  MODE_GBA_TRADE_EMU = 0x00,
  MODE_GBA_LINK = 0x01,
  MODE_GB_LINK = 0x02,
};

class UsbLink
{
public:
  UsbLink();
  ~UsbLink();
  UsbLink(const UsbLink&) = delete;
  UsbLink& operator=(const UsbLink&) = delete;

  static std::vector<DeviceInfo> List(std::string* error = nullptr);

  // Opens the first GB-Link found (or the one at bus/address when nonzero).
  bool Open(std::string* error, uint8_t bus = 0, uint8_t address = 0);
  void Close();
  bool IsOpen() const { return m_handle != nullptr; }
  bool IsNewFirmware() const { return m_new_firmware; }
  const DeviceInfo& Info() const { return m_info; }

  void SetLog(LogFn log) { m_log = std::move(log); }
  // Hex-dump every transfer through the log (for hardware bring-up).
  void SetTrace(bool on) { m_trace = on; }

  bool SendCommand(const std::vector<uint8_t>& bytes);
  bool WriteBytes(const uint8_t* data, size_t len);
  // Reads up to max_len bytes from the data channel; returns bytes read, 0 on timeout/error.
  size_t ReadBytes(uint8_t* out, size_t max_len, int timeout_ms);
  // Reads one packet from the command-status channel (new firmware only).
  size_t ReadStatus(uint8_t* out, size_t max_len, int timeout_ms);

  bool SetVoltage3V3();
  bool SetMode(uint8_t mode);
  bool SetTimingConfig(uint32_t us_between_transfers, uint8_t bytes_per_transfer);

private:
  void Trace(const char* what, const uint8_t* data, size_t len);

  libusb_context* m_ctx = nullptr;
  libusb_device_handle* m_handle = nullptr;
  DeviceInfo m_info;
  bool m_new_firmware = false;
  int m_interface = -1;
  uint8_t m_ep_cmd_out = 0, m_ep_cmd_in = 0, m_ep_out = 0, m_ep_in = 0;
  bool m_trace = false;
  LogFn m_log;
};

std::string Hex(const uint8_t* data, size_t len, size_t max = 64);
}  // namespace GBLink
