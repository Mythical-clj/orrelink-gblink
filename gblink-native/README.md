# gblink-native

Native (no browser) USB client for the GB-Link adapter. Ported from GB-Link/gb-cart-doctor-web (GPL-3.0).

* `UsbLink`  – libusb transport (command/data endpoints, same as the WebUSB client)
* `Spi32`    – 32-bit exchanges
* `Multiboot` – sends the dumper program to the GBA
* `GbaCartClient` – READ-ONLY: cart info + save dump (no restore/erase code exists here)
* `probe_main.cpp` – `gblink-probe list | info | dump <out.sav>`

`payload/gba-cart-dumper_mb.gba` is the unmodified Cart Doctor dumper (see LICENSE file next to it).
