// PLACEHOLDER. The Build XD Netplay workflow overwrites this file with the real
// orrelink-agent_mb.gba bytes (built from gba-agent/ with devkitARM) before compiling.
// With the placeholder, controller mode is unavailable and Dolphin falls back to the
// stock dumper payload for cartridge reads.
#include <cstddef>

namespace GBLink::Data
{
extern const unsigned char kAgentPayload[];
extern const size_t kAgentPayloadSize;
const unsigned char kAgentPayload[] = {0};
const size_t kAgentPayloadSize = 0;
}  // namespace GBLink::Data
