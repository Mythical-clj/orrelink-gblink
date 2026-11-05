// 32-bit SPI exchange helpers over the GB-Link data channel (port of multiboot.js
// Spi32 / Spi32Batch). Every transfer swaps one big-endian word each way.
#pragma once
#include <cstdint>
#include <algorithm>
#include <optional>
#include <vector>

#include "InputCommon/GBLink/UsbLink.h"

namespace GBLink
{
// Words per USB write: 14 x 4 = 56 bytes, under the 64-byte packet size.
constexpr size_t kBatchWords = 14;

// Returns 0 on a short/failed read, exactly like the browser client.
uint32_t Spi32(UsbLink& usb, uint32_t value);
// Throws nothing; returns std::nullopt if the link dropped.
std::optional<std::vector<uint32_t>> Spi32Batch(UsbLink& usb, const std::vector<uint32_t>& words,
                                                int read_timeout_ms = 3000);
}  // namespace GBLink
