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
#include <string>

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
  uint8_t buf[64];
  usb.SendCommand({CMD_GET_FIRMWARE_INFO});
  size_t n = usb.ReadStatus(buf, sizeof(buf), 1000);
  Log("firmware info (0x0F): " + (n ? Hex(buf, n) : std::string("<no reply>")));
  usb.SendCommand({CMD_GET_CABLE_TYPE});
  n = usb.ReadStatus(buf, sizeof(buf), 1000);
  Log("cable type (0x4A): " + (n ? Hex(buf, n) : std::string("<no reply>")) + "  [4a 00 = GBA cable, 4a 01 = GBC cable]");
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
  usb.SetVoltage3V3();

  const std::vector<uint8_t> payload(kDumperPayload, kDumperPayload + kDumperPayloadSize);
  Log("Sending the dumper program to the GBA (this does not touch the cartridge)...");
  if (!Multiboot(usb, payload, &err, Log, wait_s))
  {
    Log("MULTIBOOT FAILED: " + err);
    return 4;
  }

  GbaCartClient cart(usb, Log);
  try
  {
    if (!cart.Ping(15000))
    {
      Log("Dumper did not answer after multiboot.");
      return 5;
    }
    const CartInfo info = cart.ReadInfo();
    if (info.no_cart)
    {
      Log("No cartridge detected. On the GBA multiboot screen hold START+SELECT at the logo so the cart stays inserted/readable, then retry.");
      return 6;
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

int main(int argc, char** argv)
{
  g_log.open("gblink-probe.log", std::ios::app);
  Log("gblink-probe - GB-Link native client prototype");
  if (argc < 2)
  {
    Log("usage: gblink-probe list | info [--trace] | dump <out.sav> [--trace] [--wait seconds]");
    return 1;
  }
  const std::string cmd = argv[1];
  bool trace = false;
  int wait_s = 60;
  std::string out;
  for (int i = 2; i < argc; i++)
  {
    if (!std::strcmp(argv[i], "--trace"))
      trace = true;
    else if (!std::strcmp(argv[i], "--wait") && i + 1 < argc)
      wait_s = std::atoi(argv[++i]);
    else if (out.empty())
      out = argv[i];
  }
  if (cmd == "list")
    return CmdList();
  if (cmd == "info")
    return CmdInfo(trace);
  if (cmd == "dump" && !out.empty())
    return CmdDump(out, trace, wait_s);
  Log("bad arguments");
  return 1;
}
