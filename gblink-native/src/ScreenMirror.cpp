// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScreenMirror.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace GBLink
{
namespace
{
// Palette index = (r * 7 + g) * 6 + b with r 0..5, g 0..6, b 0..5.
constexpr int kLevelsR = 6, kLevelsG = 7, kLevelsB = 6;

uint16_t PaletteColor(int index)
{
  const int b = index % kLevelsB;
  const int g = (index / kLevelsB) % kLevelsG;
  const int r = index / (kLevelsB * kLevelsG);
  const int r5 = (r * 31 + kLevelsR / 2 - 1) / (kLevelsR - 1);
  const int g5 = (g * 31 + kLevelsG / 2 - 1) / (kLevelsG - 1);
  const int b5 = (b * 31 + kLevelsB / 2 - 1) / (kLevelsB - 1);
  return uint16_t(r5 | (g5 << 5) | (b5 << 10));
}

// 5-bit-per-channel input (the GBA only has 15 bits anyway) to palette index.
const std::array<uint8_t, 32768>& Lut()
{
  static const std::array<uint8_t, 32768> lut = [] {
    std::array<uint8_t, 32768> t{};
    for (int i = 0; i < 32768; i++)
    {
      const int r5 = i & 31, g5 = (i >> 5) & 31, b5 = (i >> 10) & 31;
      const int r = (r5 * (kLevelsR - 1) + 15) / 31;
      const int g = (g5 * (kLevelsG - 1) + 15) / 31;
      const int b = (b5 * (kLevelsB - 1) + 15) / 31;
      t[i] = uint8_t((r * kLevelsG + g) * kLevelsB + b);
    }
    return t;
  }();
  return lut;
}

inline uint8_t IndexOf(uint32_t px)
{
  const uint32_t r = (px & 0xFF) >> 3, g = ((px >> 8) & 0xFF) >> 3, b = ((px >> 16) & 0xFF) >> 3;
  return Lut()[r | (g << 5) | (b << 10)];
}

uint32_t Word(const uint8_t* p)
{
  uint32_t v;
  std::memcpy(&v, p, 4);  // little-endian host
  return v;
}
}  // namespace

ScreenMirror::ScreenMirror() : m_pending(kWidth * kHeight, 0), m_sent(kWidth * kHeight, 0)
{
}

uint16_t ScreenMirror::Quantized555(uint32_t xbgr)
{
  return PaletteColor(IndexOf(xbgr));
}

void ScreenMirror::SubmitFrame(const uint32_t* xbgr, size_t count)
{
  if (count != size_t(kWidth) * kHeight)
    return;
  std::lock_guard lock(m_mutex);
  for (size_t i = 0; i < count; i++)
    m_pending[i] = IndexOf(xbgr[i]);
  m_have_frame = true;
}

void ScreenMirror::Reset()
{
  std::lock_guard lock(m_mutex);
  m_palette_sent = false;
  m_inflight = {};
  std::fill(m_sent.begin(), m_sent.end(), uint8_t(0xFF));  // matches nothing, so every tile differs
  m_cursor = 0;
}

bool ScreenMirror::NextPacket(std::vector<uint32_t>* words, size_t max_tiles)
{
  max_tiles = std::clamp<size_t>(max_tiles, 1, 16);
  std::lock_guard lock(m_mutex);
  words->clear();
  m_inflight = {};

  if (!m_palette_sent)
  {
    // 256 colours * 2 bytes = 512 bytes = 8 chunks of 64 bytes.
    std::array<uint16_t, 256> pal{};
    for (int i = 0; i < kLevelsR * kLevelsG * kLevelsB; i++)
      pal[i] = PaletteColor(i);
    for (uint32_t c = 0; c < 8; c++)
    {
      words->push_back(kPalTileBase + c);
      for (int w = 0; w < 16; w++)
      {
        const uint32_t lo = pal[c * 32 + w * 2], hi = pal[c * 32 + w * 2 + 1];
        words->push_back(lo | (hi << 16));
      }
    }
    m_inflight.palette = true;
    return true;
  }
  if (!m_have_frame)
    return false;

  size_t found = 0;
  const size_t start = m_cursor;
  for (size_t n = 0; n < size_t(kTiles) && found < max_tiles; n++)
  {
    const size_t t = (start + n) % kTiles;
    const size_t tx = t % kTilesX, ty = t / kTilesX;
    bool differs = false;
    for (int r = 0; r < 8 && !differs; r++)
    {
      const size_t off = (ty * 8 + r) * kWidth + tx * 8;
      differs = std::memcmp(&m_pending[off], &m_sent[off], 8) != 0;
    }
    if (!differs)
      continue;
    words->push_back(uint32_t(t));
    for (int r = 0; r < 8; r++)
    {
      const size_t off = (ty * 8 + r) * kWidth + tx * 8;
      m_inflight.tile_bytes.insert(m_inflight.tile_bytes.end(), &m_pending[off], &m_pending[off] + 8);
      words->push_back(Word(&m_pending[off]));
      words->push_back(Word(&m_pending[off + 4]));
    }
    m_inflight.tile_index.push_back(uint16_t(t));
    found++;
    m_cursor = (t + 1) % kTiles;
  }
  return found > 0;
}

void ScreenMirror::PacketAcked()
{
  std::lock_guard lock(m_mutex);
  if (m_inflight.palette)
  {
    m_palette_sent = true;
    // The real GBA still shows its old picture: every tile needs sending once.
    std::fill(m_sent.begin(), m_sent.end(), uint8_t(0xFF));
  }
  // Record what was packed, not the newest frame: newer frames may have arrived since.
  for (size_t i = 0; i < m_inflight.tile_index.size(); i++)
  {
    const size_t t = m_inflight.tile_index[i];
    const size_t tx = t % kTilesX, ty = t / kTilesX;
    for (int r = 0; r < 8; r++)
    {
      const size_t off = (ty * 8 + r) * kWidth + tx * 8;
      std::memcpy(&m_sent[off], &m_inflight.tile_bytes[i * 64 + r * 8], 8);
    }
  }
  m_inflight = {};
}

void ScreenMirror::PacketFailed()
{
  std::lock_guard lock(m_mutex);
  m_inflight = {};
}
}  // namespace GBLink
