// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace GBLink
{
// The OrreLink agent: Cart Doctor's dumper plus controller mode. Size 0 when not embedded.
const uint8_t* AgentPayload(size_t* size);
// Stock Cart Doctor dumper (cartridge reads only).
const uint8_t* DumperPayload(size_t* size);
// 240x160 Mode 4 picture (palette + pixels) the controller-mode GBA displays.
const uint8_t* StandbyImage(size_t* size);
}  // namespace GBLink
