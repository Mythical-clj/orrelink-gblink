// gblink-probe: hardware bring-up + read-only cartridge save dumper for the GB-Link adapter.
//   gblink-probe list                    list adapters
//   gblink-probe info                    open, query firmware/cable, print raw replies
//   gblink-probe dump <out.sav> [--trace] [--wait N]   multiboot the dumper, read cart info + save
// Everything is mirrored into gblink-probe.log next to the exe so a failed run can be sent back.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <string>
#include <thread>

#include "GbaCartClient.h"
#include "Multiboot.h"
#include "UsbLink.h"

extern const unsigned char kDumperPayload[];
extern const unsigned long kDumperPayloadSize;

using namespace GBLink;

static std::ofstream g_log;
static void Log(const std::string& s)
{
  std::cout << s << std::endl;
  if (g_log)
    g_log << s << std::endl;
}

static void PrintDevice(const DeviceInfo& d)
{
  char b[256];
  std::snprintf(b, sizeof(b), "  %04x:%04x bcd=%04x bus=%u addr=%u  %s / %s  serial=%s  (%s firmware)",
                d.vid, d.pid, d.bcd, d.bus, d.address, d.manufacturer.c_str(), d.product.c_str(),
                d.serial.c_str(), d.IsNewFirmware() ? "new" : "old");
  Log(b);
}

static int CmdList()
{
  std::string err;
  const auto devs = UsbLink::List(&err);
  Log("GB-Link adapters found: " + std::to_string(devs.size()) + (err.empty() ? "" : " (" + err + ")"));
  for (const auto& d : devs)
    PrintDevice(d);
  return devs.empty() ? 2 : 0;
}

static int CmdInfo(bool trace)
{
  UsbLink usb;
  usb.SetLog(Log);
  usb.SetTrace(trace);
  std::string err;
  if (!usb.Open(&err))
  {
    Log("OPEN FAILED: " + err);
    return 2;
  }
  PrintDevice(usb.Info());
  usb.DrainData();
  uint8_t buf[64];
  const auto ask = [&](uint8_t cmd, const char* name) {
    usb.SendCommand({cmd});
    size_t n = usb.ReadBytes(buf, sizeof(buf), 600);
    std::string where = "data channel";
    if (!n)
    {
      n = usb.ReadStatus(buf, sizeof(buf), 600);
      where = "status channel";
    }
    Log(std::string(name) + ": " + (n ? Hex(buf, n) + "  (on " + where + ")" : std::string("<no reply>")));
  };
  ask(CMD_GET_FIRMWARE_INFO, "firmware info (0x0F)");
  ask(CMD_GET_CABLE_TYPE, "cable type (0x4A; 0 = GBA cable, 1 = GBC cable)");
  return 0;
}

static std::string g_agent_path;
static std::string g_image_path;
static int g_cable = -1;
static bool g_loaded = false;

static int CmdPad(bool trace, int wait_s, int seconds)
{
  UsbLink usb;
  usb.SetLog(Log);
  usb.SetTrace(trace);
  std::string err;
  if (!usb.Open(&err))
  {
    Log("OPEN FAILED: " + err);
    return 2;
  }
  usb.DrainData();
  usb.SetVoltage3V3();
  if (g_cable >= 0)
    usb.SetCableOverride(uint8_t(g_cable));
  usb.DrainData();
  std::vector<uint8_t> payload;
  if (g_agent_path.empty())
  {
    Log("--agent <orrelink-agent_mb.gba> is required for pad mode (download it from the Actions artifact).");
    return 1;
  }
  {
    std::ifstream f(g_agent_path, std::ios::binary);
    payload.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (payload.empty())
  {
    Log("could not read " + g_agent_path);
    return 1;
  }
  if (!Multiboot(usb, payload, &err, Log, wait_s))
  {
    Log("MULTIBOOT FAILED: " + err);
    return 4;
  }
  GbaCartClient cart(usb, Log);
  try
  {
    if (!g_image_path.empty())
    {
      std::ifstream f(g_image_path, std::ios::binary);
      std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const auto ti = std::chrono::steady_clock::now();
      cart.EnterPadModeWithImage(img);
      Log("Picture sent in " +
          std::to_string(int(std::chrono::duration<double>(std::chrono::steady_clock::now() - ti).count() * 1000)) +
          " ms");
    }
    else
      cart.EnterPadMode();
    Log("Controller mode on. Press buttons on the GBA (" + std::to_string(seconds) + " s)...");
    static const char* names[10] = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"};
    uint16_t last = 0xFFFF;
    int bad = 0;
    long polls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds))
    {
      uint16_t keys = 0;
      uint32_t raw = 0;
      if (!cart.PollPad(&keys, &raw))
      {
        char b[64];
        std::snprintf(b, sizeof(b), "unexpected reply 0x%08x", raw);
        if (++bad < 5)
          Log(b);
        if (bad > 50)
          break;
        continue;
      }
      polls++;
      if (keys != last)
      {
        last = keys;
        std::string s = "keys:";
        for (int i = 0; i < 10; i++)
          if (keys & (1 << i))
            s += std::string(" ") + names[i];
        if (!keys)
          s += " (none)";
        Log(s);
      }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log("polls: " + std::to_string(polls) + "  (" + std::to_string(int(polls / secs)) + "/s)");
    cart.LeavePadMode();
    Log("left controller mode");
  }
  catch (const GbaCartError& e)
  {
    Log(std::string("PAD FAILED [") + e.code + "]: " + e.what());
    return 7;
  }
  return 0;
}

static int CmdDump(const std::string& out_path, bool trace, int wait_s)
{
  UsbLink usb;
  usb.SetLog(Log);
  usb.SetTrace(trace);
  std::string err;
  if (!usb.Open(&err))
  {
    Log("OPEN FAILED: " + err);
    return 2;
  }
  PrintDevice(usb.Info());
  if (!usb.IsNewFirmware())
  {
    Log("This adapter reports old-style firmware; the native client needs the new firmware (update at launcher.gblink.io).");
    return 3;
  }
  usb.DrainData();
  usb.SetVoltage3V3();
  if (g_cable >= 0)
    usb.SetCableOverride(uint8_t(g_cable));
  usb.DrainData();

  if (!g_loaded)
  {
    const std::vector<uint8_t> payload(kDumperPayload, kDumperPayload + kDumperPayloadSize);
    Log("Sending the dumper program to the GBA (this does not touch the cartridge)...");
    if (!Multiboot(usb, payload, &err, Log, wait_s))
    {
      Log("MULTIBOOT FAILED: " + err);
      return 4;
    }
  }
  else
  {
    Log("--loaded: assuming the dumper is already running on the GBA");
  }

  GbaCartClient cart(usb, Log);
  try
  {
    if (!cart.Ping(15000))
    {
      Log("Dumper did not answer after multiboot.");
      return 5;
    }
    CartInfo info;
    const auto cart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    for (int attempt = 1;; attempt++)
    {
      info = cart.ReadInfo();
      if (!info.no_cart)
        break;
      Log("No cartridge detected (try " + std::to_string(attempt) +
          "). Insert the cartridge into the GBA now (power stays on) and wait; retrying...");
      if (std::chrono::steady_clock::now() > cart_deadline)
      {
        Log("Giving up: no cartridge seen. Check the GBA screen and re-seat the cartridge, then run again with --loaded.");
        return 6;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    Log("Cartridge: '" + info.title + "' code=" + info.code + " maker=" + info.maker +
        " rom=" + std::to_string(info.game_size) + " save=" + std::to_string(info.save_size));
    int last = -1;
    const auto data = cart.DumpSave([&](size_t d, size_t t) {
      const int pct = int(d * 10 / t);
      if (pct != last)
      {
        last = pct;
        Log("save read " + std::to_string(pct * 10) + "%");
      }
    });
    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    Log("Wrote " + std::to_string(data.size()) + " bytes to " + out_path);
  }
  catch (const GbaCartError& e)
  {
    Log(std::string("DUMP FAILED [") + e.code + "]: " + e.what());
    return 7;
  }
  return 0;
}

static int CmdTeam(const std::string& out_path, bool trace, int wait_s)
{
  UsbLink usb;
  usb.SetLog(Log);
  usb.SetTrace(trace);
  std::string err;
  if (!usb.Open(&err))
  {
    Log("OPEN FAILED: " + err);
    return 2;
  }
  usb.DrainData();
  usb.SetVoltage3V3();
  if (g_cable >= 0)
    usb.SetCableOverride(uint8_t(g_cable));
  usb.DrainData();
  if (g_agent_path.empty())
  {
    Log("--agent <orrelink-agent_mb.gba> is required for team mode.");
    return 1;
  }
  std::vector<uint8_t> payload;
  {
    std::ifstream f(g_agent_path, std::ios::binary);
    payload.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (payload.empty())
  {
    Log("could not read " + g_agent_path);
    return 1;
  }
  if (!g_loaded && !Multiboot(usb, payload, &err, Log, wait_s))
  {
    Log("MULTIBOOT FAILED: " + err);
    return 4;
  }
  GbaCartClient cart(usb, Log);
  try
  {
    if (!cart.Ping(15000))
    {
      Log("Agent did not answer after multiboot.");
      return 5;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto since = [&] {
      return std::to_string(int(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000)) + " ms";
    };
    CartInfo info;
    const auto deadline = t0 + std::chrono::seconds(wait_s);
    for (;;)
    {
      info = cart.BeginRanges();
      if (!info.no_cart)
        break;
      Log("No cartridge detected; retrying...");
      if (std::chrono::steady_clock::now() > deadline)
        return 6;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    Log("Cartridge '" + info.title + "' code=" + info.code + " save=" + std::to_string(info.save_size) +
        " (ready after " + since() + ")");
    const auto t1 = std::chrono::steady_clock::now();
    const auto data = cart.ReadGen3Team(info.save_size);
    Log("Team read took " +
        std::to_string(int(std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() * 1000)) +
        " ms");
    cart.EndRanges();
    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    Log("Wrote partial save image (" + std::to_string(data.size()) + " bytes) to " + out_path);
  }
  catch (const GbaCartError& e)
  {
    Log(std::string("TEAM READ FAILED [") + e.code + "]: " + e.what());
    return 7;
  }
  return 0;
}

int main(int argc, char** argv)
{
  g_log.open("gblink-probe.log", std::ios::app);
  Log("gblink-probe - GB-Link native client prototype");
  if (argc < 2)
  {
    Log("usage: gblink-probe list | info [--trace] | dump <out.sav> [--trace] [--wait seconds] [--cable auto|gba|gbc] [--loaded] | pad --agent <file.gba> [--seconds N] [--image xd-standby.bin] | team <out.sav> --agent <file.gba>");
    return 1;
  }
  const std::string cmd = argv[1];
  bool trace = false;
  int wait_s = 60;
  int seconds = 60;
  std::string out;
  for (int i = 2; i < argc; i++)
  {
    if (!std::strcmp(argv[i], "--trace"))
      trace = true;
    else if (!std::strcmp(argv[i], "--agent") && i + 1 < argc)
      g_agent_path = argv[++i];
    else if (!std::strcmp(argv[i], "--image") && i + 1 < argc)
      g_image_path = argv[++i];
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
      seconds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--loaded"))
      g_loaded = true;
    else if (!std::strcmp(argv[i], "--cable") && i + 1 < argc)
    {
      const std::string c = argv[++i];
      g_cable = c == "auto" ? 0 : c == "gba" ? 1 : c == "gbc" ? 2 : -1;
    }
    else if (!std::strcmp(argv[i], "--wait") && i + 1 < argc)
      wait_s = std::atoi(argv[++i]);
    else if (out.empty())
      out = argv[i];
  }
  if (cmd == "list")
    return CmdList();
  if (cmd == "info")
    return CmdInfo(trace);
  if (cmd == "pad")
    return CmdPad(trace, wait_s, seconds);
  if (cmd == "team" && !out.empty())
    return CmdTeam(out, trace, wait_s);
  if (cmd == "dump" && !out.empty())
    return CmdDump(out, trace, wait_s);
  Log("bad arguments");
  return 1;
}
