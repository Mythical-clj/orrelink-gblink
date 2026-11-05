#include "Multiboot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "Spi32.h"

namespace GBLink
{
namespace
{
void Sleep(int ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void FoldCrc(uint32_t& crc, uint32_t word)
{
  for (int b = 0; b < 32; b++)
  {
    const uint32_t bit = (crc ^ word) & 1;
    crc = (crc >> 1) ^ (bit ? 0xC37B : 0);
    word >>= 1;
  }
}

std::string H(uint32_t v)
{
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08x", v);
  return b;
}
}  // namespace

bool Multiboot(UsbLink& usb, const std::vector<uint8_t>& rom, std::string* error,
               const LogFn& log, int wait_s)
{
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (rom.size() > 0x40000)
    return fail("multiboot ROM larger than 256 KB");

  size_t fsize = rom.size();
  std::vector<uint8_t> padded(fsize + 0x10, 0);
  std::copy(rom.begin(), rom.end(), padded.begin());

  if (usb.IsNewFirmware())
  {
    usb.SetMode(MODE_GB_LINK);
    Sleep(100);
  }
  if (!usb.SetTimingConfig(36, 4))
    return fail("could not set timing (need new GB-Link firmware, VID 2FE3)");

  log("Waiting for the GBA (it must be at the multiboot screen: no cart inserted, or hold START+SELECT at the logo)");
  uint32_t recv = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
  int attempts = 0;
  do
  {
    const uint32_t raw = Spi32(usb, 0x6202);
    recv = raw >> 16;
    Sleep(10);
    attempts++;
    if (attempts <= 5 || attempts % 100 == 0)
      log("SPI poll #" + std::to_string(attempts) + ": sent 0x6202, got " + H(raw));
    if (std::chrono::steady_clock::now() > deadline)
      return fail("timed out waiting for the GBA's multiboot handshake (0x7202)");
  } while (recv != 0x7202);
  log("GBA detected, starting transfer");

  Spi32(usb, 0x6102);

  std::vector<uint32_t> header;
  for (size_t i = 0; i < 0xC0; i += 2)
    header.push_back(padded[i] | (uint32_t(padded[i + 1]) << 8));
  if (!Spi32Batch(usb, header))
    return fail("header transfer failed");

  Spi32(usb, 0x6200);
  Spi32(usb, 0x6202);
  Spi32(usb, 0x63D1);
  const uint32_t token = Spi32(usb, 0x63D1);
  if ((token >> 24) != 0x73)
    return fail("handshake failed (got " + H(token) + ")");
  log("Handshake ok");

  uint32_t crc_a = (token >> 16) & 0xFF;
  uint32_t seed = 0xFFFF00D1u | (crc_a << 8);
  crc_a = (crc_a + 0x0F) & 0xFF;
  Spi32(usb, 0x6400 | crc_a);

  fsize = (fsize + 0x0F) & ~size_t(0x0F);
  const uint32_t token2 = Spi32(usb, uint32_t((fsize - 0x190) / 4));
  const uint32_t crc_b = (token2 >> 16) & 0xFF;
  uint32_t crc_c = 0xC387;

  log("Sending " + std::to_string(fsize) + " bytes");
  int last_progress = -1;
  for (size_t i = 0xC0; i < fsize;)
  {
    const size_t n = std::min(kBatchWords, (fsize - i) / 4);
    std::vector<uint32_t> batch;
    for (size_t w = 0; w < n; w++)
    {
      const size_t off = i + w * 4;
      uint32_t dat = padded[off] | (uint32_t(padded[off + 1]) << 8) |
                     (uint32_t(padded[off + 2]) << 16) | (uint32_t(padded[off + 3]) << 24);
      FoldCrc(crc_c, dat);
      seed = seed * 0x6F646573u + 1;
      dat = seed ^ dat ^ (0xFE000000u - uint32_t(off)) ^ 0x43202F2Fu;
      batch.push_back(dat);
    }
    const auto rx = Spi32Batch(usb, batch);
    if (!rx)
      return fail("USB read timeout during transfer at byte " + std::to_string(i));
    for (size_t w = 0; w < n; w++)
    {
      const uint32_t off = uint32_t(i + w * 4);
      const uint32_t chk = ((*rx)[w] >> 16) & 0xFFFF;
      if (chk != (off & 0xFFFF))
        return fail("transmission error at byte " + std::to_string(off));
    }
    i += n * 4;
    const int progress = int(((i - 0xC0) * 10) / (fsize - 0xC0));
    if (progress > last_progress)
    {
      log("Progress " + std::to_string(progress * 10) + "%");
      last_progress = progress;
    }
  }

  FoldCrc(crc_c, 0xFFFF0000u | (crc_b << 8) | crc_a);

  Spi32(usb, 0x0065);
  const auto ack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do
  {
    recv = Spi32(usb, 0x0065) >> 16;
    Sleep(10);
    if (std::chrono::steady_clock::now() > ack_deadline)
      return fail("GBA never acknowledged the end of the transfer");
  } while (recv != 0x0075);
  Spi32(usb, 0x0066);
  Spi32(usb, crc_c & 0xFFFF);
  log("Multiboot complete");
  Sleep(1000);
  return true;
}
}  // namespace GBLink
