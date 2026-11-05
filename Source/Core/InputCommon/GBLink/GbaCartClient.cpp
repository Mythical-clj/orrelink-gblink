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
constexpr uint32_t CMD_DUMP_SAVE = 2, CMD_INFO = 6, CMD_PAD = 8;
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
  const auto words = ReadWords(0xC0 / 4);
  info.header.resize(0xC0);
  for (size_t w = 0; w < words.size(); w++)
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
