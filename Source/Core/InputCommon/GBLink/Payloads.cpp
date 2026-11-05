// Copyright 2026 OrreLink contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputCommon/GBLink/Payloads.h"

namespace GBLink::Data
{
extern const unsigned char kAgentPayload[];
extern const size_t kAgentPayloadSize;
extern const unsigned char kDumperPayload[];
extern const size_t kDumperPayloadSize;
}  // namespace GBLink::Data

namespace GBLink
{
const uint8_t* AgentPayload(size_t* size)
{
  *size = Data::kAgentPayloadSize;
  return Data::kAgentPayload;
}

const uint8_t* DumperPayload(size_t* size)
{
  *size = Data::kDumperPayloadSize;
  return Data::kDumperPayload;
}
}  // namespace GBLink
