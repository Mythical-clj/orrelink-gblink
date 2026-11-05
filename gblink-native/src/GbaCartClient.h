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
  // OrreLink agent only: sends a 240x160 Mode 4 picture (512-byte BGR555 palette followed by
  // 38400 index bytes, 38912 bytes in all), which the GBA shows, and enters controller mode on
  // top of it. Poll with PollPad afterwards exactly as after EnterPadMode.
  static constexpr size_t kImageBytes = 512 + 240 * 160;
  void EnterPadModeWithImage(const std::vector<uint8_t>& image);
  // One exchange. Returns false if the reply is not a key word (GBA left pad mode,
  // wrong program, or a garbled word). keys: bit0 A, 1 B, 2 Select, 3 Start, 4 Right,
  // 5 Left, 6 Up, 7 Down, 8 R, 9 L.
  bool PollPad(uint16_t* keys, uint32_t* raw_reply = nullptr);
  // Screen mirroring (OrreLink agent in controller mode). |tiles| holds n * kTileWords words
  // (n = 1..16): per tile its index (0..599 screen tile, 600..607 palette chunk) followed by
  // 16 words of data. One packet keeps reporting keys, so |keys| is refreshed when a key word
  // came back. Returns true when the GBA acknowledged an intact packet.
  static constexpr size_t kTileWords = 17;
  static constexpr size_t kTilesPerPacket = 16;
  static constexpr uint32_t kPalTileBase = 600;
  bool SendTiles(const std::vector<uint32_t>& tiles, uint16_t* keys);
  void LeavePadMode();

  // OrreLink agent only: cartridge info plus random access to the save (CMD_RANGES).
  // BeginRanges returns the same info as ReadInfo and, if the cartridge has a save,
  // waits until the GBA has staged it in RAM. ReadRange then fetches 256-byte-aligned
  // pieces. EndRanges returns the GBA to its idle state.
  CartInfo BeginRanges();
  std::vector<uint8_t> ReadRange(uint32_t offset, uint32_t length, const Progress& progress = {});
  void EndRanges();
  // Gen 3 (Ruby/Sapphire/FireRed/LeafGreen/Emerald) 128 KB flash saves: reads the 28
  // section footers and, from the active save slot, the two sections holding the
  // trainer info and the party (about 15 KB instead of 128 KB). Returns a save-sized image
  // with those parts filled in and zeros elsewhere. Throws GbaCartError "desync" if no valid
  // save slot is found (the caller can fall back to a full read).
  std::vector<uint8_t> ReadGen3Team(uint32_t save_size, const Progress& progress = {});
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
  static void ParseHeader(CartInfo* info, const std::vector<uint32_t>& words);
  void CheckCancel();

  UsbLink& m_usb;
  LogFn m_log;
  std::atomic<bool> m_cancel{false};
};
}  // namespace GBLink
