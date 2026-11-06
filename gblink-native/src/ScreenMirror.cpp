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

ScreenMirror::ScreenMirror() : m_pending(kWidth * kHeight, 0), m_target(kWidth * kHeight, 0)
{
  m_shadow[0].assign(kWidth * kHeight, 0xFF);
  m_shadow[1].assign(kWidth * kHeight, 0xFF);
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
  m_have_frame = false;  // wait for a fresh frame before replacing what the GBA shows
  m_inflight = {};
  m_front = 0;  // the standby picture is on page 0
  m_cycle = false;
  for (auto& s : m_shadow)
    std::fill(s.begin(), s.end(), uint8_t(0xFF));  // matches nothing, so every tile differs
}

namespace
{
bool TileDiffers(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t t)
{
  const size_t tx = t % ScreenMirror::kTilesX, ty = t / ScreenMirror::kTilesX;
  for (int r = 0; r < 8; r++)
  {
    const size_t off = (ty * 8 + r) * ScreenMirror::kWidth + tx * 8;
    if (std::memcmp(&a[off], &b[off], 8) != 0)
      return true;
  }
  return false;
}
}  // namespace

bool ScreenMirror::NextPacket(std::vector<uint32_t>* words, size_t max_tiles)
{
  max_tiles = std::clamp<size_t>(max_tiles, 1, 16);
  std::lock_guard lock(m_mutex);
  words->clear();
  m_inflight = {};

  // Nothing is touched until there is a frame to show: sending the palette alone would recolour
  // whatever the real GBA is displaying (the standby picture has its own palette).
  if (!m_have_frame)
    return false;

  const int back = 1 - m_front;
  if (!m_cycle)
  {
    // Start drawing a new frame only if it differs from what is on screen. The frame is frozen
    // so a screen that keeps changing still gets finished and shown.
    bool differs = !m_palette_sent;
    for (size_t t = 0; t < size_t(kTiles) && !differs; t++)
      differs = TileDiffers(m_pending, m_shadow[m_front], t);
    if (!differs)
      return false;
    m_target = m_pending;
    m_cycle = true;
  }

  std::vector<size_t> todo;
  for (size_t t = 0; t < size_t(kTiles); t++)
  {
    if (TileDiffers(m_target, m_shadow[back], t))
      todo.push_back(t);
  }
  const size_t take = std::min(todo.size(), max_tiles);
  const bool last = take == todo.size();  // this packet finishes the frame
  words->push_back(0);                    // header, filled in below
  for (size_t i = 0; i < take; i++)
  {
    const size_t t = todo[i];
    const size_t tx = t % kTilesX, ty = t / kTilesX;
    words->push_back(uint32_t(t));
    for (int r = 0; r < 8; r++)
    {
      const size_t off = (ty * 8 + r) * kWidth + tx * 8;
      m_inflight.tile_bytes.insert(m_inflight.tile_bytes.end(), &m_target[off], &m_target[off] + 8);
      words->push_back(Word(&m_target[off]));
      words->push_back(Word(&m_target[off + 4]));
    }
    m_inflight.tile_index.push_back(uint16_t(t));
  }
  // The palette goes out together with the flip, so the old picture is never recoloured. It
  // rides along when it fits (8 chunks), otherwise it gets a packet of its own.
  const bool with_palette = last && !m_palette_sent && take + 8 <= 16;
  if (with_palette || (last && !m_palette_sent && take == 0))
  {
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
  }
  // A last packet whose palette did not fit goes out as a tiles-only packet; the next call finds
  // nothing left to draw and sends the palette and the flip.
  m_inflight.flip = last && (m_palette_sent || m_inflight.palette);
  m_inflight.page = back;
  (*words)[0] = (m_inflight.flip ? kFlagFlip : 0) | (back ? kFlagPage1 : 0);
  return words->size() > 1 || m_inflight.flip;  // a flip-only packet is just the header
}

void ScreenMirror::PacketAcked()
{
  std::lock_guard lock(m_mutex);
  if (m_inflight.palette)
    m_palette_sent = true;
  // Record what was packed, not the newest frame.
  auto& shadow = m_shadow[m_inflight.page];
  for (size_t i = 0; i < m_inflight.tile_index.size(); i++)
  {
    const size_t t = m_inflight.tile_index[i];
    const size_t tx = t % kTilesX, ty = t / kTilesX;
    for (int r = 0; r < 8; r++)
    {
      const size_t off = (ty * 8 + r) * kWidth + tx * 8;
      std::memcpy(&shadow[off], &m_inflight.tile_bytes[i * 64 + r * 8], 8);
    }
  }
  if (m_inflight.flip)
  {
    m_front = m_inflight.page;
    m_cycle = false;
  }
  m_inflight = {};
}

void ScreenMirror::PacketFailed()
{
  std::lock_guard lock(m_mutex);
  // The real GBA may have drawn some of this packet before the answer got lost: forget what
  // those tiles hold so they are sent again.
  auto& shadow = m_shadow[m_inflight.page];
  for (const uint16_t t : m_inflight.tile_index)
  {
    const size_t tx = t % kTilesX, ty = t / kTilesX;
    for (int r = 0; r < 8; r++)
      std::memset(&shadow[(ty * 8 + r) * kWidth + tx * 8], 0xFF, 8);
  }
  m_inflight = {};
}
}  // namespace GBLink
