#include "InputCommon/GBLink/UsbLink.h"

#include <algorithm>
#include <cstdio>

#include <libusb.h>

namespace GBLink
{
namespace
{
constexpr uint16_t kVids[] = {0x2FE3, 0xCAFE, 0x239A};

bool IsGbLinkVid(uint16_t vid)
{
  return std::find(std::begin(kVids), std::end(kVids), vid) != std::end(kVids);
}

std::string StrDesc(libusb_device_handle* h, uint8_t idx)
{
  if (!h || idx == 0)
    return {};
  unsigned char buf[256];
  const int n = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof(buf));
  return n > 0 ? std::string(reinterpret_cast<char*>(buf), n) : std::string{};
}
}  // namespace

std::string Hex(const uint8_t* data, size_t len, size_t max)
{
  std::string s;
  char b[4];
  for (size_t i = 0; i < len && i < max; i++)
  {
    std::snprintf(b, sizeof(b), "%02x", data[i]);
    s += b;
    if (i + 1 < len)
      s += ' ';
  }
  if (len > max)
    s += " ...";
  return s;
}

UsbLink::UsbLink()
{
  if (libusb_init(&m_ctx) != 0)
    m_ctx = nullptr;
}

UsbLink::~UsbLink()
{
  Close();
  if (m_ctx)
    libusb_exit(m_ctx);
}

std::vector<DeviceInfo> UsbLink::List(std::string* error)
{
  std::vector<DeviceInfo> out;
  libusb_context* ctx = nullptr;
  if (libusb_init(&ctx) != 0)
  {
    if (error)
      *error = "libusb_init failed";
    return out;
  }
  libusb_device** list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++)
  {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(list[i], &d) != 0 || !IsGbLinkVid(d.idVendor))
      continue;
    DeviceInfo info;
    info.vid = d.idVendor;
    info.pid = d.idProduct;
    info.bcd = d.bcdDevice;
    info.bus = libusb_get_bus_number(list[i]);
    info.address = libusb_get_device_address(list[i]);
    libusb_device_handle* h = nullptr;
    if (libusb_open(list[i], &h) == 0)
    {
      info.product = StrDesc(h, d.iProduct);
      info.manufacturer = StrDesc(h, d.iManufacturer);
      info.serial = StrDesc(h, d.iSerialNumber);
      libusb_close(h);
    }
    else
    {
      info.product = "(could not open: driver not WinUSB/libusb?)";
    }
    out.push_back(std::move(info));
  }
  if (list)
    libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return out;
}

bool UsbLink::Open(std::string* error, uint8_t bus, uint8_t address)
{
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    Close();
    return false;
  };
  if (!m_ctx)
    return fail("libusb_init failed");

  libusb_device** list = nullptr;
  const ssize_t n = libusb_get_device_list(m_ctx, &list);
  libusb_device* found = nullptr;
  libusb_device_descriptor fd{};
  for (ssize_t i = 0; i < n && !found; i++)
  {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(list[i], &d) != 0 || !IsGbLinkVid(d.idVendor))
      continue;
    if (bus && (libusb_get_bus_number(list[i]) != bus ||
                libusb_get_device_address(list[i]) != address))
      continue;
    found = list[i];
    fd = d;
  }
  if (!found)
  {
    if (list)
      libusb_free_device_list(list, 1);
    return fail("no GB-Link adapter found (VID 2FE3/CAFE/239A)");
  }

  const int orc = libusb_open(found, &m_handle);
  m_info = {};
  m_info.vid = fd.idVendor;
  m_info.pid = fd.idProduct;
  m_info.bcd = fd.bcdDevice;
  m_info.bus = libusb_get_bus_number(found);
  m_info.address = libusb_get_device_address(found);
  libusb_free_device_list(list, 1);
  if (orc != 0)
  {
    m_handle = nullptr;
    return fail(std::string("libusb_open failed: ") + libusb_error_name(orc) +
                " (on Windows the adapter must be bound to the WinUSB driver)");
  }
  m_info.product = StrDesc(m_handle, fd.iProduct);
  m_info.manufacturer = StrDesc(m_handle, fd.iManufacturer);
  m_info.serial = StrDesc(m_handle, fd.iSerialNumber);
  m_new_firmware = m_info.IsNewFirmware();

  // The browser client resets the device first; do the same, ignoring failure.
  libusb_reset_device(m_handle);

  int rc = libusb_set_configuration(m_handle, 1);
  if (rc != 0 && rc != LIBUSB_ERROR_BUSY && rc != LIBUSB_ERROR_NOT_SUPPORTED)
    Trace("set_configuration failed (continuing)", nullptr, 0);

  libusb_config_descriptor* cfg = nullptr;
  rc = libusb_get_active_config_descriptor(libusb_get_device(m_handle), &cfg);
  if (rc != 0)
    return fail(std::string("get config descriptor failed: ") + libusb_error_name(rc));

  bool have = false;
  for (int i = 0; i < cfg->bNumInterfaces && !have; i++)
  {
    const libusb_interface& itf = cfg->interface[i];
    for (int a = 0; a < itf.num_altsetting && !have; a++)
    {
      const libusb_interface_descriptor& alt = itf.altsetting[a];
      if (alt.bInterfaceClass != 0xFF)
        continue;
      std::vector<uint8_t> ins, outs;
      for (int e = 0; e < alt.bNumEndpoints; e++)
      {
        const uint8_t addr = alt.endpoint[e].bEndpointAddress;
        if ((alt.endpoint[e].bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
          continue;
        (addr & 0x80 ? ins : outs).push_back(addr);
      }
      std::sort(ins.begin(), ins.end());
      std::sort(outs.begin(), outs.end());
      m_interface = alt.bInterfaceNumber;
      if (m_new_firmware && ins.size() >= 2 && outs.size() >= 2)
      {
        m_ep_cmd_out = outs[0];
        m_ep_cmd_in = ins[0];
        m_ep_out = outs[1];
        m_ep_in = ins[1];
        have = true;
      }
      else if (!ins.empty() && !outs.empty())
      {
        m_ep_out = outs[0];
        m_ep_in = ins[0];
        have = true;
      }
    }
  }
  libusb_free_config_descriptor(cfg);
  if (!have)
    return fail("no vendor-class (0xFF) interface with bulk endpoints found");

  libusb_set_auto_detach_kernel_driver(m_handle, 1);
  rc = libusb_claim_interface(m_handle, m_interface);
  if (rc != 0)
    return fail(std::string("claim_interface failed: ") + libusb_error_name(rc));

  if (!m_new_firmware)
  {
    // CDC-style SET_CONTROL_LINE_STATE (DTR) as the browser client sends.
    libusb_control_transfer(m_handle, 0x21, 0x22, 0x01, static_cast<uint16_t>(m_interface),
                            nullptr, 0, 1000);
  }
  if (m_log)
  {
    char b[200];
    std::snprintf(b, sizeof(b),
                  "opened %04x:%04x bcd=%04x iface=%d ep cmd out/in=%02x/%02x data out/in=%02x/%02x",
                  m_info.vid, m_info.pid, m_info.bcd, m_interface, m_ep_cmd_out, m_ep_cmd_in,
                  m_ep_out, m_ep_in);
    m_log(b);
  }
  return true;
}

void UsbLink::Close()
{
  if (m_handle)
  {
    if (m_interface >= 0)
      libusb_release_interface(m_handle, m_interface);
    libusb_close(m_handle);
    m_handle = nullptr;
  }
  m_interface = -1;
  m_lost = false;
}

void UsbLink::NoteResult(int rc)
{
  if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE)
    m_lost = true;
}

void UsbLink::Trace(const char* what, const uint8_t* data, size_t len)
{
  if (!m_trace || !m_log)
    return;
  m_log(std::string(what) + (data ? " " + Hex(data, len) : std::string{}));
}

bool UsbLink::SendCommand(const std::vector<uint8_t>& bytes)
{
  if (!m_handle)
    return false;
  const uint8_t ep = (m_new_firmware && m_ep_cmd_out) ? m_ep_cmd_out : m_ep_out;
  int sent = 0;
  const int rc = libusb_bulk_transfer(m_handle, ep, const_cast<uint8_t*>(bytes.data()),
                                      static_cast<int>(bytes.size()), &sent, 2000);
  NoteResult(rc);
  Trace("CMD  >", bytes.data(), bytes.size());
  return rc == 0 && sent == static_cast<int>(bytes.size());
}

bool UsbLink::WriteBytes(const uint8_t* data, size_t len)
{
  if (!m_handle)
    return false;
  int sent = 0;
  const int rc = libusb_bulk_transfer(m_handle, m_ep_out, const_cast<uint8_t*>(data),
                                      static_cast<int>(len), &sent, 2000);
  NoteResult(rc);
  Trace("DATA >", data, len);
  return rc == 0 && sent == static_cast<int>(len);
}

size_t UsbLink::ReadBytes(uint8_t* out, size_t max_len, int timeout_ms)
{
  if (!m_handle)
    return 0;
  int got = 0;
  const int rc = libusb_bulk_transfer(m_handle, m_ep_in, out, static_cast<int>(max_len), &got,
                                      timeout_ms);
  NoteResult(rc);
  if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
    Trace((std::string("DATA < error ") + libusb_error_name(rc)).c_str(), nullptr, 0);
  if (got > 0)
    Trace("DATA <", out, got);
  return got > 0 ? static_cast<size_t>(got) : 0;
}

size_t UsbLink::ReadStatus(uint8_t* out, size_t max_len, int timeout_ms)
{
  if (!m_handle || !m_new_firmware || !m_ep_cmd_in)
    return 0;
  int got = 0;
  libusb_bulk_transfer(m_handle, m_ep_cmd_in, out, static_cast<int>(max_len), &got, timeout_ms);
  if (got > 0)
    Trace("STAT <", out, got);
  return got > 0 ? static_cast<size_t>(got) : 0;
}

size_t UsbLink::DrainData(int quiet_ms)
{
  size_t total = 0;
  uint8_t buf[64];
  for (int i = 0; i < 64; i++)
  {
    const size_t n = ReadBytes(buf, sizeof(buf), quiet_ms);
    if (n == 0)
      break;
    total += n;
  }
  return total;
}

bool UsbLink::SetCableOverride(uint8_t sel)
{
  return m_new_firmware ? SendCommand({CMD_SET_CABLE_OVERRIDE, sel}) : true;
}

bool UsbLink::SetVoltage3V3()
{
  if (m_new_firmware)
    return SendCommand({CMD_VOLTAGE_3V3});
  return true;  // old firmware: needs the magic-prefix packet; not supported here
}

bool UsbLink::SetMode(uint8_t mode)
{
  return m_new_firmware ? SendCommand({CMD_SET_MODE, mode}) : true;
}

bool UsbLink::SetTimingConfig(uint32_t us, uint8_t bytes_per_transfer)
{
  if (!m_new_firmware)
    return false;  // old firmware path (36-byte magic packet) intentionally unsupported
  return SendCommand({CMD_SET_TIMING, static_cast<uint8_t>(us & 0xFF),
                      static_cast<uint8_t>((us >> 8) & 0xFF),
                      static_cast<uint8_t>((us >> 16) & 0xFF), bytes_per_transfer});
}
}  // namespace GBLink
