#include "InputCommon/GBLink/GbaCartClient.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "InputCommon/GBLink/Spi32.h"

namespace GBLink
{
namespace
{
constexpr uint32_t M_MAGIC = 0x47420000, M_POLL = 0, M_GO = 0x4742600D, M_CONT = 0x4742C047,
                   M_FAIL = 0x4742BAD0, M_ABORT = 0x4742AB0B;
constexpr uint32_t G_READY = 0x52454459, G_DONE = 0x444F4E45;
constexpr uint32_t CMD_DUMP_SAVE = 2, CMD_INFO = 6, CMD_PAD = 8, CMD_RANGES = 9, CMD_IMAGE = 10;
constexpr uint32_t G_OKOK = 0x4F4B4F4B;
constexpr uint32_t M_RANGE_OFF = 0x47430000, M_RANGE_LEN = 0x47440000;
constexpr uint32_t PAD_MAGIC = 0x4B500000;
constexpr size_t SECTION_WORDS = 0x40;

using Clock = std::chrono::steady_clock;
void Sleep(int ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
}  // namespace

void GbaCartClient::CheckCancel()
{
  if (m_cancel || (m_ext_cancel && *m_ext_cancel))
    throw GbaCartError("cancelled", "Cancelled");
}

std::vector<uint32_t> GbaCartClient::XferBatch(const std::vector<uint32_t>& words)
{
  auto r = Spi32Batch(m_usb, words);
  if (!r)
    throw GbaCartError("link", "USB read timeout - link lost?");
  return *r;
}

void GbaCartClient::PollReady(int timeout_ms)
{
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline)
  {
    CheckCancel();
    const uint32_t w = Xfer(M_POLL);
    if (w == G_READY)
      return;
    if (w == G_DONE)
      Xfer(M_ABORT);
    Sleep(25);
  }
  throw GbaCartError("timeout",
                     "GBA dumper not responding. Is the payload running and the cable connected?");
}

bool GbaCartClient::Ping(int timeout_ms)
{
  try
  {
    PollReady(timeout_ms);
    return true;
  }
  catch (const GbaCartError& e)
  {
    if (e.code == "cancelled")
      throw;
    return false;
  }
}

void GbaCartClient::Command(uint32_t cmd)
{
  for (int attempt = 0; attempt < 5; attempt++)
  {
    CheckCancel();
    PollReady(10000);
    if (Xfer(M_MAGIC | cmd) != G_READY)
      continue;
    if (Xfer(M_GO) == uint32_t(~(M_MAGIC | cmd)))
      return;
  }
  throw GbaCartError("desync", "Command handshake failed after retries");
}

void GbaCartClient::WaitDone(int timeout_ms)
{
  const auto started = Clock::now();
  while (Clock::now() - started < std::chrono::milliseconds(timeout_ms))
  {
    CheckCancel();
    const uint32_t w = Xfer(M_POLL);
    if (w == G_DONE)
    {
      Xfer(M_CONT);
      return;
    }
    if (w == G_READY)
      throw GbaCartError("desync", "Payload returned to idle mid-operation");
    Sleep(200);
  }
  throw GbaCartError("timeout", "Timed out waiting for the GBA");
}

std::vector<uint32_t> GbaCartClient::ReadWords(size_t n)
{
  return XferBatch(std::vector<uint32_t>(n, M_POLL));
}

std::vector<uint8_t> GbaCartClient::StreamIn(size_t total, const Progress& progress)
{
  std::vector<uint8_t> out(total);
  const size_t sections = total / 0x100;
  const std::vector<uint32_t> tx(SECTION_WORDS + 1, M_POLL);
  int retries = 0;
  for (size_t s = 0; s < sections;)
  {
    if (m_cancel || (m_ext_cancel && *m_ext_cancel))
    {
      Xfer(M_ABORT);
      throw GbaCartError("cancelled", "Cancelled");
    }
    const auto resp = XferBatch(tx);
    uint32_t xsum = 0;
    for (size_t w = 0; w < SECTION_WORDS; w++)
      xsum ^= resp[w];
    if (xsum == resp[SECTION_WORDS])
    {
      for (size_t w = 0; w < SECTION_WORDS; w++)
      {
        const uint32_t v = resp[w];  // stored little-endian, as the GBA holds it
        uint8_t* p = &out[s * 0x100 + w * 4];
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
        p[3] = uint8_t(v >> 24);
      }
      Xfer(M_CONT);
      s++;
      retries = 0;
      if (progress)
        progress(s * 0x100, total);
    }
    else
    {
      if (++retries > 50)
      {
        Xfer(M_ABORT);
        throw GbaCartError("checksum", "Section " + std::to_string(s) + " failed checksum 50 times");
      }
      Xfer(M_FAIL);
    }
  }
  return out;
}

void GbaCartClient::EnterPadMode()
{
  Command(CMD_PAD);
}

void GbaCartClient::EnterPadModeWithImage(const std::vector<uint8_t>& image)
{
  if (image.size() != kImageBytes)
    throw GbaCartError("desync", "Bad picture size");
  Command(CMD_IMAGE);
  const uint32_t len = uint32_t(image.size());
  Xfer(len);
  if (Xfer(~len) != len)
    throw GbaCartError("desync", "The GBA did not accept the picture");
  const size_t sections = image.size() / 0x100;
  std::vector<uint32_t> tx(SECTION_WORDS + 2);
  int retries = 0;
  for (size_t s = 0; s < sections;)
  {
    CheckCancel();
    uint32_t xsum = 0;
    for (size_t w = 0; w < SECTION_WORDS; w++)
    {
      const uint8_t* p = &image[s * 0x100 + w * 4];
      const uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                         (uint32_t(p[3]) << 24);
      tx[w] = v;
      xsum ^= v;
    }
    tx[SECTION_WORDS] = xsum;
    tx[SECTION_WORDS + 1] = M_CONT;  // the GBA's answer (OKOK/FAIL) arrives in this exchange
    const auto resp = XferBatch(tx);
    if (resp[SECTION_WORDS + 1] == G_OKOK)
    {
      s++;
      retries = 0;
    }
    else if (++retries > 50)
    {
      Xfer(M_ABORT);
      throw GbaCartError("checksum", "A picture section failed 50 times");
    }
  }
  // The GBA now switches its screen and starts the controller loop; give it a moment.
  Sleep(60);
}

bool GbaCartClient::PollPad(uint16_t* keys, uint32_t* raw_reply)
{
  const uint32_t w = Xfer(M_POLL);
  if (raw_reply)
    *raw_reply = w;
  if ((w & 0xFFFF0000u) != PAD_MAGIC)
    return false;
  *keys = static_cast<uint16_t>(w & 0x03FF);
  return true;
}

void GbaCartClient::LeavePadMode()
{
  Xfer(M_ABORT);
  PollReady(3000);
}

void GbaCartClient::ParseHeader(CartInfo* info_out, const std::vector<uint32_t>& words)
{
  CartInfo& info = *info_out;
  info.header.resize(0xC0);
  for (size_t w = 0; w < words.size() && w < 0x30; w++)
    std::memcpy(&info.header[w * 4], &words[w], 4);  // host is little-endian (x86/ARM64)
  const auto ascii = [&](size_t from, size_t len) {
    std::string s;
    for (size_t i = from; i < from + len; i++)
    {
      const uint8_t c = info.header[i];
      if (c == 0)
        break;
      s += (c >= 0x20 && c < 0x7F) ? char(c) : '_';
    }
    return s;
  };
  info.title = ascii(0xA0, 12);
  info.code = ascii(0xAC, 4);
  info.maker = ascii(0xB0, 2);
}

CartInfo GbaCartClient::BeginRanges()
{
  Command(CMD_RANGES);
  WaitDone(60000);
  const auto sizes = ReadWords(2);
  CartInfo info;
  if (sizes[0] == 0xFFFFFFFF)
  {
    info.no_cart = true;
    return info;
  }
  info.game_size = sizes[0];
  info.save_size = sizes[1];
  ParseHeader(&info, ReadWords(0xC0 / 4));
  if (info.save_size == 0)
    return info;  // the GBA is back at its main loop
  WaitDone(120000);  // the GBA copies the save into its RAM
  return info;
}

std::vector<uint8_t> GbaCartClient::ReadRange(uint32_t offset, uint32_t length,
                                              const Progress& progress)
{
  if (offset % 0x100 || length % 0x100 || length == 0 || offset / 0x100 > 0xFFFF ||
      length / 0x100 > 0xFFFF)
    throw GbaCartError("desync", "Bad save range");
  const uint32_t w1 = M_RANGE_OFF | (offset / 0x100);
  const uint32_t w2 = M_RANGE_LEN | (length / 0x100);
  for (int attempt = 0; attempt < 8; attempt++)
  {
    CheckCancel();
    PollReady(5000);
    Xfer(w1);
    const uint32_t echo1 = Xfer(w2);
    const uint32_t echo2 = Xfer(M_POLL);
    if (echo1 != uint32_t(~w1) || echo2 != uint32_t(~w2))
      continue;
    Xfer(M_GO);
    return StreamIn(length, progress);
  }
  throw GbaCartError("desync", "The GBA did not accept the save range request");
}

void GbaCartClient::EndRanges()
{
  Xfer(M_ABORT);
  PollReady(3000);
}

std::vector<uint8_t> GbaCartClient::ReadGen3Team(uint32_t save_size, const Progress& progress)
{
  constexpr uint32_t kSection = 4096, kPerBlock = 14, kBlock = kSection * kPerBlock;
  constexpr uint32_t kFooter = 0xFF4, kFooterBlock = 0xF00;
  constexpr uint32_t kSignature = 0x08012025;
  if (save_size < 2 * kBlock)
    throw GbaCartError("desync", "Save is too small to be a Gen 3 save");

  const size_t total = 2 * kPerBlock * 0x100 + 2 * kSection;
  size_t done_bytes = 0;
  const auto sub_progress = [&](size_t base) {
    return [&, base](size_t d, size_t) {
      if (progress)
        progress(base + d, total);
    };
  };

  std::vector<uint8_t> image(save_size, 0);
  // 1. Every section footer: id, checksum, signature, save index.
  for (uint32_t blk = 0; blk < 2; blk++)
  {
    for (uint32_t sec = 0; sec < kPerBlock; sec++)
    {
      const uint32_t off = blk * kBlock + sec * kSection + kFooterBlock;
      const auto data = ReadRange(off, 0x100);
      std::memcpy(&image[off], data.data(), data.size());
      done_bytes += 0x100;
      if (progress)
        progress(done_bytes, total);
    }
  }

  const auto rd16 = [&](size_t o) { return uint32_t(image[o]) | (uint32_t(image[o + 1]) << 8); };
  const auto rd32 = [&](size_t o) { return rd16(o) | (rd16(o + 2) << 16); };
  struct BlockInfo
  {
    bool valid = true;
    int64_t index = -1;
    int pos_of[kPerBlock];
  } info[2];
  for (uint32_t blk = 0; blk < 2; blk++)
  {
    BlockInfo& bi = info[blk];
    for (auto& p : bi.pos_of)
      p = -1;
    for (uint32_t sec = 0; sec < kPerBlock; sec++)
    {
      const size_t o = blk * kBlock + sec * kSection + kFooter;
      const uint32_t sid = rd16(o);
      const uint32_t sig = rd32(o + 4);
      const uint32_t idx = rd32(o + 8);
      if (sig != kSignature || sid >= kPerBlock || bi.pos_of[sid] != -1)
      {
        bi.valid = false;
        continue;
      }
      bi.pos_of[sid] = int(sec);
      if (bi.index == -1)
        bi.index = idx;
      else if (bi.index != int64_t(idx))
        bi.valid = false;
    }
  }
  int active = -1;
  if (info[0].valid && info[1].valid)
    active = info[1].index > info[0].index ? 1 : 0;
  else if (info[1].valid)
    active = 1;
  else if (info[0].valid)
    active = 0;
  if (active < 0)
    throw GbaCartError("desync", "No valid Gen 3 save slot found");

  // 2. Logical sections 0 (trainer info) and 1 (team and items) of the active slot.
  size_t base = done_bytes;
  for (uint32_t id = 0; id < 2; id++)
  {
    const uint32_t off = uint32_t(active) * kBlock + uint32_t(info[active].pos_of[id]) * kSection;
    const auto data = ReadRange(off, kSection, sub_progress(base));
    std::memcpy(&image[off], data.data(), data.size());
    base += kSection;
  }
  if (progress)
    progress(total, total);
  return image;
}

CartInfo GbaCartClient::ReadInfo()
{
  Command(CMD_INFO);
  WaitDone(60000);
  const auto sizes = ReadWords(2);
  CartInfo info;
  if (sizes[0] == 0xFFFFFFFF)
  {
    info.no_cart = true;
    return info;
  }
  info.game_size = sizes[0];
  info.save_size = sizes[1];
  ParseHeader(&info, ReadWords(0xC0 / 4));
  return info;
}

std::vector<uint8_t> GbaCartClient::DumpSave(const Progress& progress)
{
  Command(CMD_DUMP_SAVE);
  WaitDone(60000);
  const auto sizes = ReadWords(2);
  if (sizes[0] == 0xFFFFFFFF)
    throw GbaCartError("no-cart", "No GBA cartridge detected");
  if (sizes[1] == 0)
    throw GbaCartError("no-save", "This cartridge has no save memory");
  if (sizes[1] % 0x100 || sizes[1] > 0x20000)
    throw GbaCartError("desync", "Implausible save size " + std::to_string(sizes[1]));
  WaitDone(120000);
  return StreamIn(sizes[1], progress);
}
}  // namespace GBLink
