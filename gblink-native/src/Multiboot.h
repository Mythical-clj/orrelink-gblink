// Sends a program to a GBA over the link cable (multiboot), as a second GBA would.
// Port of multiboot.js from GB-Link/gb-cart-doctor-web (GPL-3.0).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "UsbLink.h"

namespace GBLink
{
// Returns true on success; on failure *error says where it stopped.
bool Multiboot(UsbLink& usb, const std::vector<uint8_t>& rom, std::string* error,
               const LogFn& log, int wait_for_gba_seconds = 60);
}  // namespace GBLink
