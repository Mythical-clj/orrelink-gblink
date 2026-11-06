// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Turns the emulated GBA's 240x160 frames into small tile-update packets for the real GBA.
// A fixed 252-colour palette keeps the mapping stateless, so an unchanged screen costs nothing
// and a changed menu cursor costs a handful of 8x8 tiles.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace GBLink
{
class ScreenMirror
{
public:
  static constexpr int kWidth = 240, kHeight = 160, kTilesX = 30, kTilesY = 20, kTiles = 600;
  static constexpr size_t kTileWords = 17;  // index + 16 data words
  static constexpr uint32_t kPalTileBase = 600;
  static constexpr uint32_t kFlagFlip = 0x100, kFlagPage1 = 0x200;  // packet header flags

  ScreenMirror();

  // Thread-safe. |xbgr| is kWidth*kHeight pixels, 0x??BBGGRR (mGBA's order).
  void SubmitFrame(const uint32_t* xbgr, size_t count);

  // The next packet or false when the real GBA is up to date. words[0] is a header
  // (kFlagFlip, kFlagPage1), followed by up to |max_tiles| tiles of kTileWords words each.
  // The tiles of a frame are drawn on the page the real GBA is not showing; the last packet of a
  // frame carries kFlagFlip, so the whole picture appears at once. Call PacketAcked /
  // PacketFailed afterwards.
  bool NextPacket(std::vector<uint32_t>* words, size_t max_tiles);
  void PacketAcked();
  void PacketFailed();

  // Forget what the real GBA shows (new connection): palette and all tiles go out again.
  void Reset();

  // Test hook: the colour an emulated pixel becomes on the real GBA, as BGR555.
  static uint16_t Quantized555(uint32_t xbgr);

private:
  struct Inflight
  {
    std::vector<uint16_t> tile_index;
    std::vector<uint8_t> tile_bytes;  // 64 per tile, the pixels as packed
    bool palette = false;
    bool flip = false;
    int page = 0;
  };

  std::mutex m_mutex;
  std::vector<uint8_t> m_pending;  // newest frame, palette indices
  std::vector<uint8_t> m_target;   // the frame being drawn (frozen so it can finish)
  std::vector<uint8_t> m_shadow[2];  // what each of the real GBA's two pages holds
  int m_front = 0;                 // the page the real GBA shows
  bool m_cycle = false;            // a frame is being drawn on the back page
  bool m_have_frame = false;
  bool m_palette_sent = false;
  Inflight m_inflight;
};
}  // namespace GBLink
