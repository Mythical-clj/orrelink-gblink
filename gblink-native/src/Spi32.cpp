#include "Spi32.h"

namespace GBLink
{
uint32_t Spi32(UsbLink& usb, uint32_t v)
{
  const uint8_t tx[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  if (!usb.WriteBytes(tx, 4))
    return 0;
  uint8_t rx[4];
  if (usb.ReadBytes(rx, 4, 2000) < 4)
    return 0;
  return (uint32_t(rx[0]) << 24) | (uint32_t(rx[1]) << 16) | (uint32_t(rx[2]) << 8) | rx[3];
}

std::optional<std::vector<uint32_t>> Spi32Batch(UsbLink& usb, const std::vector<uint32_t>& words,
                                                int read_timeout_ms)
{
  std::vector<uint32_t> out;
  out.reserve(words.size());
  for (size_t i = 0; i < words.size(); i += kBatchWords)
  {
    const size_t n = std::min(kBatchWords, words.size() - i);
    uint8_t tx[kBatchWords * 4];
    for (size_t w = 0; w < n; w++)
    {
      const uint32_t v = words[i + w];
      tx[w * 4] = uint8_t(v >> 24);
      tx[w * 4 + 1] = uint8_t(v >> 16);
      tx[w * 4 + 2] = uint8_t(v >> 8);
      tx[w * 4 + 3] = uint8_t(v);
    }
    if (!usb.WriteBytes(tx, n * 4))
      return std::nullopt;
    // The reply can be split over several USB packets: keep reading.
    uint8_t rx[kBatchWords * 4];
    size_t got = 0;
    while (got < n * 4)
    {
      const size_t part = usb.ReadBytes(rx + got, n * 4 - got, read_timeout_ms);
      if (part == 0)
        return std::nullopt;
      got += part;
    }
    for (size_t w = 0; w < n; w++)
    {
      out.push_back((uint32_t(rx[w * 4]) << 24) | (uint32_t(rx[w * 4 + 1]) << 16) |
                    (uint32_t(rx[w * 4 + 2]) << 8) | rx[w * 4 + 3]);
    }
  }
  return out;
}
}  // namespace GBLink
