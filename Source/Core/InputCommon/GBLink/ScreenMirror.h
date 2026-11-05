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

  ScreenMirror();

  // Thread-safe. |xbgr| is kWidth*kHeight pixels, 0x??BBGGRR (mGBA's order).
  void SubmitFrame(const uint32_t* xbgr, size_t count);

  // The next packet (up to |max_tiles| tiles, each kTileWords words) or false when the real GBA
  // is up to date. Call PacketAcked / PacketFailed afterwards.
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
  };

  std::mutex m_mutex;
  std::vector<uint8_t> m_pending;  // newest frame, palette indices
  std::vector<uint8_t> m_sent;     // what the real GBA shows
  bool m_have_frame = false;
  bool m_palette_sent = false;
  size_t m_cursor = 0;
  Inflight m_inflight;
};
}  // namespace GBLink
