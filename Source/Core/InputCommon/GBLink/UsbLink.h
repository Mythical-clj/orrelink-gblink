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

#ifdef ANDROID
#include <jni.h>
#endif

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
  // Where the adapter is plugged in, e.g. "1-4.2" (bus-port.port): the same USB port gives the
  // same key every time, even though the bus address changes on replug.
  std::string Key() const { return std::to_string(bus) + "-" + port_path; }
  std::string port_path;
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
  CMD_SET_CABLE_OVERRIDE = 0x49,
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
  // True once a transfer failed because the adapter went away.
  bool Lost() const { return m_lost; }
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

  // Discards any stale bytes waiting on the data channel (replies to earlier commands
  // can land there and would otherwise shift every later exchange).
  size_t DrainData(int quiet_ms = 60);
  bool SetCableOverride(uint8_t sel);  // 0 auto, 1 GBA cable, 2 GBC cable (session only)
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
  bool m_lost = false;
  void NoteResult(int libusb_rc);
  // One bulk transfer on either backend (libusb, or the Java connection on Android).
  int BulkXfer(uint8_t ep, uint8_t* data, int len, int* done, int timeout_ms);
  std::string m_android_key;  // Android only: the UsbManager device name
  LogFn m_log;
};

std::string Hex(const uint8_t* data, size_t len, size_t max = 64);

#ifdef ANDROID
// Android has no libusb here: the Kotlin class GBLinkUsb owns the UsbManager connection and this
// module calls into it. Init is called once from Java with that class.
namespace AndroidBridge
{
void Init(JNIEnv* env, jclass usb_class);
}
#endif
}  // namespace GBLink
