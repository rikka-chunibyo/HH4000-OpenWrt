# Configurable OpenWrt front panel

Current state: OpenWrt is now installed persistently. See [the installation and verification record](30-installed-openwrt.md). The RAM tests below remain historical evidence.

The hybrid OpenWrt RAM image now includes its own front-panel renderer and menu service. Configure it at **System -> Front Panel** in LuCI, or through `/etc/config/hh_frontpanel`. The page provides live wake, navigation, menu and custom-message controls as well as configuration.

## Defaults

| Control | Default |
| --- | --- |
| Home screen | OpenWrt HH4000 and the selected network's IPv4 address |
| Pages | Home, LAN port links, Wi-Fi radio state, system uptime/load/memory, custom message |
| UP / DOWN | Previous / next page, or previous / next menu item |
| OK | Open menu, then select the highlighted page |
| Main button | Wake / home / back; one action per press, no hold-to-reboot action |
| Screen timeout | 60 seconds; the first active button press wakes the screen |
| Text brightness | 60 percent grayscale intensity |
| Main ring | White when the selected local network has an address; blue while waiting; red when status updates fail |
| UP / OK / DOWN lights | White while the screen is awake, off while asleep |
| Custom message | Welcome; second line empty |

Each button can be reassigned to previous, next, select, menu, home, screen toggle, wake or no action. Main ring supports white/blue/red/off/status. OK supports white/amber/red/off. Navigation lighting can follow the screen, stay on, or stay off. Internet connectivity and PON are not required for a healthy local-network indication. Text accepts up to 20 printable ASCII characters per line.

The runtime screen settings are separate from the installed U-Boot greeting in doc 27. No Linux boot-environment setter was added.

## Hardware and runtime

The small internal MCU owns the large main button and some power/thermal signals. It is connected over I2C; the existing mcu-core, mcu-i2c and mcu-keys drivers expose input events. UP/OK/DOWN are SoC GPIO buttons. The external Arduino is only the UART/power-control development connection and is not required by the panel service.

The service uses the real ARM32 GPIO ioctl ABI through the backed-up glibc loader. `/dev/led` is provided by **drv_gpio.ko**, major 150 minor 24. It is not the general `/dev/bsp_dev` node. Button state reads do not register or steal GPIO callbacks. Main EV_KEY 247 repeat values are coalesced; GPIO presses are debounced. No stock Bell button/reboot daemon is started.

Screen drawing uses our own 8x16 bitmap renderer through `/dev/fb0`, 168x42x8, respecting the queried stride. One complete framebuffer write avoids repeated full-panel transfers. The stock blanking quirk is honored: FBIOBLANK=1 normal, 4 blank; 0 would turn all pixels on. Brightness adjusts rendered grayscale, not the panel hardware contrast register.

`hh-frontpanel-init` loads BSP GPIO, board data, drv_gpio, SSD1322 and MCU drivers. The BSP driver needs valid permanent_param on UBI device 0 or 1. The helper attaches boot MTD 2 if necessary, checks the identity format before insertion, and never changes the identity volume. An initial test without this dependency hit the stock driver's invalid fallback address and reset; the guarded path is the implemented startup. No flash image was changed for front-panel integration.

Procd supervises the panel and a separate read-only network-status provider. Runtime files and the owner-only control socket live under `/var/run/hh-frontpanel`. The renderer does not run shell commands from configuration or button actions. LuCI permissions allow this UCI package, the non-sensitive state JSON and the fixed control helper. Saved settings trigger a managed reload. Disabling the service blanks the OLED and turns its lights off. Disabling only the screen leaves the remaining controls configurable.

## Files and build

- Source: `src/openwrt-hh4000/frontpanel/frontpanel.c` and `font8x16.h` (ASCII subset of Linux's GPL font).
- Configuration/startup/UI: `src/openwrt-hh4000/files/{etc,usr,www}/`.
- Build/tests: `python3 tools/build_frontpanel.py` on dev, using the project-local ARM32 compiler and backed-up libc. Host checks run with AddressSanitizer and UndefinedBehaviorSanitizer.
- Build image: `python3 tools/build_openwrt_integrated.py --lab` preserves the private lab radio configuration. Normal non-lab builds default to disabled radios.
- Artifacts: `work/frontpanel/`, including the prior known-good RAM image and overlay package. The final initrd remains `work/openwrt-integrate/openwrt-integrated.uImage`.

Twenty-three state, configuration, renderer, GPIO debounce and MCU repeat checks passed on dev; the earlier matching test set also ran on the router. Physical camera tests show the home address, white ring/navigation lights, and a custom two-line message with blue main and amber OK lights. The user tested UP/OK/DOWN/main on the running service; final observed input source was hardware/main. RPC tests used UCI set/apply/confirm, verified active settings, temporarily remapped UP to next, selected the Message page and restored defaults. Eight-second test blanking and wake passed. The LuCI authenticated route and JavaScript asset were served successfully; no shared-browser visual test was performed in this pass.

A fresh RAM boot started the service automatically, loaded the framebuffer and MCU input, and restored the defaults. Final checks confirmed all eight bridge members (five Ethernet, three Wi-Fi), all three radio up states, the independent watchdog feeder, and exact default LED channel states. The wired check returned 5/5 replies with zero loss. Results are recorded in `work/frontpanel/hardware-results.json` and `final-check.raw`. This is still a RAM initramfs: runtime settings reset on a power cycle until persistent OpenWrt storage is implemented. The installed U-Boot greeting remains persistent. PON research is parked as requested.

Current configurable/programming modes, templates, API and validation: [36-programmable-front-panel.md](36-programmable-front-panel.md). The earlier fixed five-page description above is historical.
