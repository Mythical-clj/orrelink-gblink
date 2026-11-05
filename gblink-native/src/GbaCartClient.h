// Client for the Cart Doctor GBA dumper payload once it is running on the GBA.
// Port of the READ-ONLY parts of gba_cart.js (GB-Link/gb-cart-doctor-web, GPL-3.0).
// Restore / erase / cart-run commands are deliberately NOT ported: this code can
// never write to a cartridge.
#pragma once
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "UsbLink.h"

namespace GBLink
{
class GbaCartError : public std::runtime_error
{
public:
  // timeout | no-cart | no-save | desync | checksum | cancelled | link
  GbaCartError(std::string code, const std::string& msg) : std::runtime_error(msg), code(std::move(code)) {}
  std::string code;
};

struct CartInfo
{
  bool no_cart = false;
  uint32_t game_size = 0, save_size = 0;
  std::string title, code, maker;
  std::vector<uint8_t> header;  // 0xC0 bytes
};

class GbaCartClient
{
public:
  using Progress = std::function<void(size_t done, size_t total)>;
  GbaCartClient(UsbLink& usb, LogFn log) : m_usb(usb), m_log(std::move(log)) {}

  void Cancel() { m_cancel = true; }
  bool Ping(int timeout_ms = 3000);

  // OrreLink agent only (orrelink-agent_mb.gba, not the stock Cart Doctor dumper).
  // Controller mode: after EnterPadMode the GBA answers every exchange with its key
  // state. Leave it with LeavePadMode before issuing any other command.
  void EnterPadMode();
  // One exchange. Returns false if the reply is not a key word (GBA left pad mode,
  // wrong program, or a garbled word). keys: bit0 A, 1 B, 2 Select, 3 Start, 4 Right,
  // 5 Left, 6 Up, 7 Down, 8 R, 9 L.
  bool PollPad(uint16_t* keys, uint32_t* raw_reply = nullptr);
  void LeavePadMode();
  CartInfo ReadInfo();
  // Dumps the cartridge save (EEPROM 512B/8K, SRAM 32K, Flash 64K/128K).
  std::vector<uint8_t> DumpSave(const Progress& progress = {});

private:
  std::vector<uint32_t> XferBatch(const std::vector<uint32_t>& words);
  uint32_t Xfer(uint32_t w) { return XferBatch({w})[0]; }
  void PollReady(int timeout_ms);
  void Command(uint32_t cmd);
  void WaitDone(int timeout_ms);
  std::vector<uint32_t> ReadWords(size_t n);
  std::vector<uint8_t> StreamIn(size_t total_bytes, const Progress& progress);
  void CheckCancel();

  UsbLink& m_usb;
  LogFn m_log;
  std::atomic<bool> m_cancel{false};
};
}  // namespace GBLink
