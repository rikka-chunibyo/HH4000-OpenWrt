# 04 – Peripherals: OLED screen, buttons, EFM8 MCU, LEDs, GPIO map, I2C, sensors, USB, watchdog, voice

Target: Bell Home Hub 4000 (Sagemcom F@ST 5689, "SagemcomFast5689_Bell"), BCM6858, stock kernel 4.1.52-5.02L.07 (arm64 kernel, 32-bit ARM userspace).

**Legend.** **[V]** = verified, with the file, symbol, offset or log line cited. **[I]** = inferred, with the reasoning given. **[U]** = unknown or needs hardware testing.
`$P` = `/path/to/HH4000-OpenWrt`. Scratch, scripts and decompiles are in `$P/work/periph/`. In that tree:
- `dec/*.c` holds the Ghidra output and `dis/*.dis` the objdump output.
- `efm8.bin`/`efm8.dis` is the MCU image, `decodebsp.py` the GPIO-table decoder and `bsp_bell_gpio.txt` its output.
- `uboot_dtb.dts` is the DTB embedded in the Sagemcom U-Boot (new in pass 2).
- `mainline/` holds the mainline sources fetched for comparison: torvalds/linux master **05441d697f7f** (2026-11-03, v7.4-rc1 merge window). File names use `_` for `/`.
- `oled_test/` holds the OLED test tool and a DRM driver sketch.

---

## 0. Summary

| Peripheral | Hardware (evidence) | Stock driver | OpenWrt approach | Effort |
|---|---|---|---|---|
| Front OLED | SSD1322, **167(→168) x 42** 4-bit grey. GDDRAM column window starts at 0x28 (SEG160). HSSPI **CS5** (pad GPIO54 func 2), SPI mode 3, 3.3 MHz. D/C = GPIO25, RST = GPIO75 (active-low), VCC-enable = GPIO74 (active-high). U-Boot initialises and lights it | U-Boot `scom,ssd1322` + `ssd1322.ko` (Sagemcom fbdev, legacy Broadcom SPI API) | Mainline `ssd130x` "solomon,ssd1322" is **not usable** (SSD1327 command set, no 0x5C, 2-px columns, no column offset; §1.7). Use `oled_test` (spidev, ready) or the mipi-dbi DRM sketch | S (userspace) / M (driver) |
| Front buttons UP/OK/DOWN + rear RESET | SoC GPIO **13 = UP, 14 = DOWN, 15 = OK**, 59 = RESET. Active-low, polled. The mapping is resolved end-to-end (§2) | `bsp_gpio_lkm.ko` polling + `drv_gpio.ko` `/dev/butN` | `gpio-keys-polled` | S |
| Front LEDs | GPIO 5 = UP white, **6 = DOWN white**, 7/8 = OK red/yellow, **9 = OK white**, 76/77/78 = power white/red/blue. Active-high | bsp / `drv_gpio` `/dev/led` | `gpio-leds` | S |
| EFM8BB10F8G MCU | I2C-0 @ **0x78**. Owns the "hard" (power) button, a green LED, a power-enable output, a thermal-alert latch. IRQ to SoC GPIO52, **active-low at the SoC** | `mcu-core/-i2c/-keys.ko` (stock actually polls it every 10 ms) | Userspace i2c daemon or small driver (§3). **No keep-alive needed** | S–M |
| Temp sensors | 2x TMP102 @ 0x48/0x49 on i2c-0 | `tmp102` hwmon | Mainline `tmp102` | S |
| I2C | 2 BSC controllers: **0xff85a800** (i2c-0: TMP102 x2, FUSB301, MCU, M02099) and **0xff802100** (i2c-1: M02180). Register map identical to brcmstb BSC | built-in `i2c-bcm63000` | Mainline `i2c-brcmstb` (`brcm,brcmper-i2c`, `clock-frequency = <97500>`, polling) (§7) | S |
| USB | One port, USB3 SS + USB2. Type-C via **FUSB301** @ i2c-0 0x25, Source-only, 1.5 A | `bcm_usb.ko`, `fusb301.ko` | `phy-brcm-usb` (`brcm,bcm4908-usb-phy`; same MDIO fixups) + generic xhci/ehci/ohci. USB power domain needs work [U]. FUSB301 from userspace | M |
| Watchdog | SoC WDT0 @ `0xff802780`, 50 MHz, max 85.9 s. WDT1 @ `0xff8027c0`. **U-Boot 2019.07 starts WDT0 before booting Linux** [I, 60 s default] | `hard_watchdog.ko` (85 s / 5 s pacify) | `bcm7038_wdt` (`brcm,bcm6345-wdt`) at 0xff802780 with a 50 MHz clock. **Mainline bcm6858.dtsi's watchdog@ff800428 is wrong for 6858** (§8) | S (must be in early boot) |
| LED controller | 0xff800800 drives the **8 Ethernet LEDs** (GPIO 62/63/65/66/68/69/71/72, pinmux func 6, HW-LED mode from LPORT) | Broadcom built-in `bcm_led` + LPORT LED | `leds-bcm63138` (SW mode only) + netdev trigger, or a HW-mode extension (§4.4) | S–M |
| Buzzer / fan | **None** (no GPIO, PWM, MCU command or strings) | – | – | – |
| Voice/FXS | Si3228x dual FXS daughterboard, reset GPIO82 | `slicslac.ko` + `dsphal.ko` | Not supportable. Hold in reset | – |
| GPIO/pinmux | DIR `0xff800500+4n`, DATA `0xff800520+4n`, 117 GPIOs. Pinmux TEST_PORT block @ `0xff800550` (EN_LSB/DATA_MSB/DATA_LSB/CMD). Same register layout as BCM4908. GPIO = func 5 | Broadcom built-in | `brcm,bcm6345-gpio` x4 banks. A 6858 table for `pinctrl-bcm4908` is needed. U-Boot already muxes the OLED/LED pins (§4.3) | S / M |

---

## 1. Front OLED screen (SSD1322)

### 1.1 Device-tree nodes and driver identity [V]
**Linux stock DTB** (`$P/work/devicetree.dts`) has a top-level node, not under the SPI controller:
```
screen { compatible="scom,ssd1322"; default-state="keep"; spi-cs=<5>; spi-clk=<0x325aa0>;
         pin-cs=<0x36 2 0>; pin-dc=<0x19 5 1>; pin-rst=<0x4b 5 0>; pin-pwr=<0x4a 5 1>;
         screen-width=<0xa7>; screen-height=<0x2a>; screen-offset=<0>; start-row=<0>; start-col=<0x28>; };
```
**U-Boot DTB** (new, pass 2). This is the Sagemcom U-Boot 2019.07 `sc-3.31.1`, `sc_f5689`, DTB at offset 0xe91c0 of `ubi/img_mtd2/mtd2.bin/img-967626498_vol-uboot.ubifs`, extracted to `work/periph/uboot_dtb.dts`. It has a *real* SPI child, with pinctrl:
```
spi-controller@ff801000 { compatible = "brcm,bcm6328-hsspi"; num-cs = <8>; clocks = <hsspi-pll = 2 x 200 MHz periph-osc>;
  screen@5 { compatible = "scom,ssd1322"; reg = <5>; pinctrl-0 = <&screen-pins &screen-spi>;
     pin-cs = <&gpio1 22 1>;  pin-dc = <&gpio0 25 0>;  pin-rst = <&gpio2 11 1>;  pin-pwr = <&gpio2 10 0>;
     screen-width = <167>; screen-height = <42>; max-spi-frequency = <3300000>; start-row = <0>; start-col = <0x28>; }; };
pinctrl@ff800548 { compatible = "brcm,bcm63xx-pinctrl"; brcm,pins-count = <126>; brcm,functions-count = <7>;
  screen-pins { pins = "25", "75", "74"; function = "5"; };   screen-spi { pins = "54"; function = "2"; }; };
```
U-Boot strings include `SSD1322`, `scom,ssd1322`, "fail to setup SPI" and a bitmap font. **U-Boot initialises and lights the panel**, which is why the kernel node says `default-state="keep"` [V; this upgrades the pass-1 inference]. The kernel module is `ssd1322.ko`: "Solomon SSD1322 framebuffer device (for bcm63xx)" v0.4.1, Paul-Erwan RIO (Sagemcom), GPL. Kernel log [V]: `width=167 (upscaled to 168) height:42 vmem:7056`, then `keep display`.

### 1.2 Pins and SPI [V]
`ssd1322.ko` (`FUN_001009d0`/`FUN_00100378`) reads each `pin-*` property as `<gpio pinmux-func active-high>`. It stores `gpio | 0x8000` (BP_ACTIVE_LOW) when cell2 = 0 and calls `bcm_set_pinmux(pin, func)`.

| Signal | SoC pad | pinmux func | polarity | Evidence |
|---|---|---|---|---|
| CS# | GPIO54 | 2 = HSSPI SS5 | active-low (HW CS) | `pinmux_table_6858.c` `{bp_usSpiSlaveSelectNum, 5, 54, 54\|BP_PINMUX_VAL_2}`. U-Boot `screen-spi` |
| D/C# | GPIO25 | 5 (GPIO) | 0 = command, 1 = data | both DTs |
| RES# | GPIO75 | 5 | active-low | both DTs + BSP "Oled Reset" act-low |
| VCC enable | GPIO74 | 5 | active-high | both DTs. The power-up order matches the datasheet's VCC step exactly (§1.3) [I→strong] |
| SCLK/MOSI/MISO | GPIO102/103/104 | 1 | – | `pinmux_table_6858.c` `BP_PINMUX_FNTYPE_HS_SPI` |

The stock module calls `BcmSpiReserveSlave2(bus=1 HSSPI, cs=5, 3300000 Hz, mode=3, ctrlState=0xa0000000)`. Each command or data byte group is one 8-bit `spi_transfer` via `BcmSpiSyncMultTrans`, preceded by `kerSysSetGpioState(dc, is_data)` (`.text 0x0–0xbc`). The HSSPI PLL is 400 MHz (stock DTS `hsspi-pll`; U-Boot `fixed-factor` 2 x 200 MHz periph-osc). HSSPI IRQ = GIC SPI 37 (`6858_intr.h` `INTERRUPT_PER_HS_SPIM_IRQ`, same as the stock DTS and mainline dtsi).

### 1.3 Power-up / init sequence, checked against the SSD1322 datasheet [V code / V datasheet]
The bytes come from the `ssd1322.ko` disassembly, `.text 0x450–0x9c8`. Pass 2 re-extracted every `mov w4,#imm` and `strb`, and they all match. The column on the right is the meaning from the Solomon SSD1322 datasheet (Rev 1.x, command table §9).
```
PWR=0 (GPIO74); RST=1; RST=0; msleep(1); RST=1; PWR=1; msleep(200)
  ; datasheet power-on: VDD up -> RES# low >=100 us -> high -> VCC on -> AFh, SEG/COM on after ~200 ms
C FD D 12        Command lock: 0x12 = unlock (reset default is unlocked anyway)
C A4             Entire display OFF (all GS0). A5 = all ON, A6 = normal, A7 = inverse
C A0 D 04 D 11   Re-map. A=0x04: A[0]=0 horizontal increment, A[1]=0 no column remap,
                 A[2]=1 NIBBLE remap, A[4]=0 COM0->COM[N-1] (NOT reversed, pass-1 text was wrong), A[5]=0 no odd/even split.
                 B=0x11: B[4]=1 dual-COM mode, B[0]=1 (required)
C A1 D 00        Start line 0
C A2 D 00        Display offset 0
C A6             Normal display
C A9             Exit partial display
C AB D 01        Function select: internal VDD regulator
C B1 D FF        Phase 1 = 31 DCLK, phase 2 = 15 DCLK (maximum)
C B3 D D2        Clock: divide 2^2 = 4, oscillator frequency 0xD
C B4 D A0 D FD   Display enhancement A: external VSL (A[1:0]=00), enhanced low-GS quality (B=0xFD)
C B6 D 08        Second precharge period 8 DCLK
C B9             Default linear grey-scale table
C BB D 1F        Precharge voltage 0.60 x VCC
C BE D 07        VCOMH 0.86 x VCC
C C1 D FF        Contrast current (max)
C C7 D 0F        Master contrast current (max, 16/16)
C CA D 2A        MUX ratio N+1 = 43. This is a constant in the code, not derived from height (42). Harmless: row 42 is never written
C D1 D 82 D 20   Display enhancement B (same value as Newhaven NHD-3.12 reference init)
-- clear GDDRAM: 15 c0 c1 ; 75 r0 r1 ; 5C ; then height x width/2 zero bytes
C AF or C AE     depending on default-state ("on" → AF; anything except "keep" → AE; "keep" skips everything)
```
**Geometry [V].** The column window is `start-col .. start-col + width/4 - 1` = **0x28..0x51**: SSD1322 column addresses are 4-pixel units, so this is SEG160..SEG327 of the 480-SEG GDDRAM. The row window is 0..41. 168 px = 84 bytes per row and 3528 bytes per frame.

**Pixel packing [V].** `FUN_00100f30` does `out = (p[2i] & 0xF0) | (p[2i+1] >> 4)`, so the left pixel goes in the **high** nibble. That is the correct order with A0 bit 2 (nibble remap) set. **Every RAM write is preceded by command 0x5C** (Write RAM). On the SSD1322 this is mandatory; without it, data bytes are ignored.

**SPI [V datasheet].** The SSD1322 samples SDIN on the rising SCLK edge, MSB first, and D/C# on the 8th bit, so mode 0 or mode 3 both work. The minimum serial clock period is 100 ns (≤10 MHz). Stock uses 3.3 MHz.

**Power-off (datasheet):** send AE, switch VCC off (GPIO74 = 0), and wait ≥100 ms before removing VDD.

### 1.4 Userspace interface on stock [V]
- **`/dev/fb0`.** 8 bpp grey, `xres=168 yres=42 line_length=168`, smem 7056 bytes. It uses `fb_deferred_io` (1 jiffy), and a write triggers a full refresh.
- **`fb_blank`.** 0 → A5 (all pixels ON), 1–3 → A6 (normal), 4 → A4 (all OFF). bell-screend uses `/sys/class/graphics/fb0/blank` to turn the screen up/down.
- **ioctl `0x40014602`** sends `C1 <arg>` (contrast).
- **sysfs `display_on`.** 0 → AE, non-zero → AF.
- **`/usr/share/screend/images/loading.raw`** (7056 bytes) is a raw **168x42 8-bpp** frame (the boot "loading" image) [V size]. `oled_test --raw` can show it.

### 1.5 Mainline `ssd130x` "solomon,ssd1322": why it does not work [V source]
The compatible exists: `drivers/gpu/drm/solomon/ssd130x-spi.c:133` (`"solomon,ssd1322"` → `SSD1322_ID`) and the binding `solomon,ssd132x.yaml:15`. The SSD1322 variant is only `{480x128, SSD132X_FAMILY}` (`ssd130x.c:232-236`), and the SSD132x family shares one SSD1327-style code path. Citations are to `$P/work/periph/mainline/drivers_gpu_drm_solomon_ssd130x.c` @ 05441d697f7f.

| Line(s) | Mainline does | SSD1322 needs | Effect |
|---|---|---|---|
| 627-654 `ssd132x_init` | `0x81 0x80` contrast | contrast is `C1` (0x81 is undefined) | ignored or garbage |
| 637 | `A0 0x53` single byte | `A0 A B`, two bytes. The next command byte (`A1`) is consumed as B | wrong remap / dual-COM |
| 642 | `A8 h-1` MUX | MUX is `CA`; `A8` = **Enter Partial Display** | display broken |
| 643-651 | `B1 55`, `B3 01`, `B6 01`, `BC 08`, `D5 62` | `BC`/`D5` don't exist on SSD1322 (precharge V = `BB`, enhancement = `B4`/`D1`) | wrong analog setup |
| 49, 881-930 `ssd132x_update_rect` | `SSD132X_SEGMENT_WIDTH 2`: column = x/2, 2-px units | 4-px column units | wrong window |
| 914 | `ssd132x_set_col_range(col, …)` with **no `col_offset`** (only the SSD130x path at 826/1033 uses it) | offset 0x28 | image lands at SEG0, off-glass |
| 928 | `(n2 & 0xf0) \| (n1 >> 4)`: left pixel in the **low** nibble | left pixel high nibble (with A0 bit2 = 1) | pixel pairs swapped |
| 914-933 | sends data right after the 15/75 commands, **no `0x5C`** | Write-RAM 0x5C before data | **nothing is written** |
| 2026-2033 `ssd130x_update_bl` | `0x81 <contrast>` | `C1 <contrast>` | backlight control broken |

The SSD132x series (v4, Oct 2023, Javier Martinez Canillas) was tested only on an SSD1327 ([cover letter](https://lists.freedesktop.org/archives/dri-devel/2023-October/426609.html), [LWN](https://lwn.net/Articles/947160)).

**Upstream fix (patch description).**
1. Split out an `SSD1322` family (or give the SSD132x family per-variant ops): `ssd1322_init` with the §1.3 table, 2-byte `A0`, `CA` for MUX, `C1` for contrast.
2. Add `SSD1322_SEGMENT_WIDTH 4` and apply `col_offset` (from `solomon,col-offset`) in `ssd132x_set_col_range`.
3. Emit `0x5C` before the data in update_rect and clear_screen.
4. Make the nibble order follow the remap setting.

That is roughly 150 lines of change, and it needs an SSD1322 owner to test. Effort M.

### 1.6 OpenWrt: ready-to-use spec and tools
**Spec (all [V] unless noted).**

| Item | Value |
|---|---|
| Controller | SSD1322, 4-wire SPI + D/C |
| Bus | HSSPI @0xff801000 (`brcm,bcm6858-hsspi`, `brcm,bcmbca-hsspi-v1.0`). **Mainline driver = `spi-bcm63xx-hsspi`** (`spi-bcm63xx-hsspi.c:937-938` matches `brcm,bcm6328-hsspi` and `brcm,bcmbca-hsspi-v1.0`; `spi-bcmbca-hsspi.c:615` only matches `-v1.1`; pass 1 named the wrong driver), CS5 |
| SPI | mode 3, 8 bit, MSB first, 3.3 MHz (≤10 MHz) |
| GPIO | D/C GPIO25 (`gpio0 25`), RES# GPIO75 (`gpio2 11`, active-low), VCC-EN GPIO74 (`gpio2 10`, active-high) |
| Pinmux | 25/74/75 → func 5, 54 → func 2. **The stock U-Boot already does this** (`screen-pins`/`screen-spi`), so OpenWrt booted from stock U-Boot needs no pinctrl for the OLED [V U-Boot DT; I that the values persist into Linux] |
| Window | columns 0x28..0x51 (`15 28 51`), rows 0..41 (`75 00 29`), then `5C` and 3528 bytes |
| Pixel | 4 bpp, 2 px/byte, left pixel = high nibble |
| Visible | 167 x 42 (column 167 is off-glass) |
| Init | §1.3 table, then `A6`, `AF` |

**Userspace test program.** `$P/work/periph/oled_test/oled_test.c` (plus a `Makefile`; a static `oled_test.aarch64` builds cleanly with `aarch64-linux-gnu-gcc`).
- It uses spidev plus the GPIO chardev uAPI v2 directly, with no libgpiod dependency.
- It finds lines automatically on mainline `brcm,bcm6345-gpio` banks (chip label `ff8005XX.gpio`). Override with `--dc/--rst/--pwr CHIP:LINE`.
- Optional `--pinmux` pokes `0xff800554/58/5c` via `/dev/mem`.
- Modes:
  - `--init` runs the full stock power-up and init.
  - `--keep` drives a panel that U-Boot already lit.
  - `--pattern` draws a border, 16 grey steps, a checker (to check nibble order) and text.
  - `--text`, `--raw` (168x42 8 bpp, e.g. stock `loading.raw`), `--pgm`, `--contrast`, `--on/--off/--allon/--normal/--alloff`.
- DT for spidev: `&hsspi { status="okay"; spidev@5 { compatible = "<a spidev-allowed compatible>"; reg = <5>; spi-max-frequency = <3300000>; spi-cpol; spi-cpha; }; };` or bind via `driver_override`.

**Kernel driver sketch.** `$P/work/periph/oled_test/ssd1322-hh4000-drm.c` is a mipi-dbi tiny DRM driver modelled line-by-line on mainline `drivers/gpu/drm/sitronix/st7586.c` (the in-tree template for a D/C-SPI controller with an odd grey format). It contains the §1.3 init, 4-px clip alignment, `col_offset`, `0x5C` and high-nibble packing, a `power-supply` regulator for GPIO74, and fbdev emulation for `/dev/fb0`. It is **not compiled or tested**. Effort to finish: S–M. DT:
```dts
/ { oled_vcc: regulator-oled { compatible = "regulator-fixed"; regulator-name = "oled-vcc";
      gpio = <&gpio2 10 GPIO_ACTIVE_HIGH>; enable-active-high; regulator-boot-on; startup-delay-us = <200000>; }; };
&hsspi { status = "okay";
  display@5 { compatible = "sagemcom,hh4000-oled"; reg = <5>; spi-max-frequency = <3300000>; spi-cpol; spi-cpha;
    dc-gpios = <&gpio0 25 GPIO_ACTIVE_HIGH>; reset-gpios = <&gpio2 11 GPIO_ACTIVE_LOW>;
    power-supply = <&oled_vcc>; solomon,col-offset = <40>; }; };
```
Keep `regulator-boot-on` so the U-Boot-lit panel isn't power-cycled before the driver probes.

### 1.7 bell-screend: what to replicate [V strings/disasm, I where noted]

Follow-up: [20-display-library.md](20-display-library.md) independently traces the reusable toolkit, its ABI/dependencies, and off-device drawing probes.

- **Stack.** 32-bit ARM C++. `libScreenApi.so` is a small widget toolkit on SDL 1.2 (`System`, `View`, `VMenu`, `VYNMenu` yes/no menu, `ButtonItem`, `TextArea`, `ImageArea`, `LoadBar`, `LoadingArea`, `TimerArea`, `ScrollArea`).
- **Video mode.** `SDL_SetVideoMode(w, h, bpp, SDL_HWPALETTE)` on fbcon `/dev/fb0` (`libScreenApi` `System::start` @0x56f0).
- **Font.** Only `/usr/share/screend/fonts/OpenSans-SemiBold.ttf` is referenced. TextArea's default size is 14 (`mov r2,#14` @0x527c). bell-screend sets size 10 on 9 TextAreas and size 30 on one [V immediate stores to TextArea+0x4c], so most text is **OpenSans SemiBold 10 px** and the big number (speed test result) is 30 px [I].
- **Images actually referenced by bell-screend.** `back-arrow.png` (14x14) and `icon_test_normal/focused.png` (10x10). The 167-px assets (`menu_item_background*.png` 167x14, `up_bar/down_bar.png` 167x6, `outline_box.png` 167x20) are this panel's menu skin. The 240x320 assets and the 120-frame WPS animation (110x110) belong to other Sagemcom screens and don't fit this panel [I].
- **Screens** (EN/FR strings):
  - **Main menu** (`PanMenu`, VMenu, 8 lambda actions): Service status, Wi-Fi password, Connect Fibe TV receiver, Connect Wi-Fi device (WPS), Modem password, Test internet speed, Switch to English/Passer au français, Reset to factory default, Reboot modem, Close.
  - **Yes/No confirms** (`VYNMenu`): "Reset modem to factory default?", "Are you sure? All settings will be reset.", "Restart the modem?".
  - **Notifications** (`PanNotif`):
    - Boot progress: "Connecting to Bell", "Configuring services", "Configuring Wi-Fi", "Your Wi-Fi network is".
    - Activation errors: "Visit Bell.ca/activation", "Call 310-Bell", "Error #".
    - Fibre and optical state: initial, operational, standby, ranging, emergency stop, and so on.
    - Voice errors and the temperature warning.
    - Upgrade: "Modem upgrading", "Upgrade failed".
    - Reset-button hints: "Keep pushing 5 sec to reboot" and "Keep pushing 10 sec to factory reset".
  - **WPS flows** (`PanMenuWpsData`): connected, cancelled and failed states.
  - **Speed test** ("Mbps download/upload").
- **Data sources.** All data comes over Sagemcom ISM (`libIntfScreenmanager`): `recvMsg_status`, `bootUp`, `wpsEvents`, `speedtest`, `default_password`, `displayMessage`, `screenNotification`, `languageChanged`. Outgoing requests are `sendReq_setLed`, `enableWifi`, `wpsEvents`, `speedtest`, `credentialRequest`, `setLanguage` and `intfReboot_sendReq_reboot`. For OpenWrt, replace these with ubus/uci queries (WAN state, SSID/key, firmware version, temperature from hwmon).
- **Buttons.** The chain is `/dev/butN` → `libbellosm-smu` `CButtonOsm` → `CScreenOsm::{up,down,ok}ButtonEvent` → ISM `screenEvents` → bell-screend `handle_btn_event`. UP and DOWN are `VMenu::prevItem/nextItem`, OK is `trigItem`. Inactivity unstacks panels, and the screen turns off via `fb0/blank` ("Turn Screen DOWN") [V strings; the timeout value is not extracted, U].
- **LEDs from the UI.** `CScreenOsm::setBtnLed` maps the screen-manager LED enum 1..7 → HAL LED {27 power red, 28 power blue, 22 OK white, 23 OK red, 24 OK yellow, 21 UP white, 25 DOWN white} [V byte table @0x83595 in libbellosm-smu.so]. The focused button's LED is lit.
- **Replicating it on OpenWrt:** a small C daemon with a framebuffer renderer is a possible route; implementation size has not been established. Read `KEY_UP/KEY_DOWN/KEY_ENTER` from gpio-keys-polled, render 168x42 grey with a 10–14 px font, drive the button LEDs via `/sys/class/leds`, and blank after inactivity.

---

## 2. Buttons

### 2.1 Resolution of the OK/DOWN conflict: **GPIO14 = DOWN, GPIO15 = OK; LED6 = DOWN white, LED9 = OK white** [V chain, high confidence]
There are three sources. Two DTs (the Linux `buttons`/`leds` nodes and the U-Boot `gpio-hog` line-names `led-whiteok` = GPIO6 / `led-whitedown` = GPIO9) say the opposite of the BSP table. Neither DT is functional: Linux has no driver for `scom,bcm63xx-gpio`, and U-Boot only drives the LEDs low and does not read the buttons. The production path, traced end to end in pass 2, is:

1. **BSP board table** (`bsp_gpio_data.ko` `board_info_5689_bell`): GPIO13 → name-id 0x74 "Button UP", GPIO14 → 0x75 "Button DOWN", GPIO15 → 0x78 "Button OK".
2. **`bsp_gpio_lkm.ko` button-index table** at `.data+0x2548` (`kSysButtonManaged`/`kSysGetButtonState` index → name-id, then `bsp_gpio_get_num()` reverse lookup): idx0 0x71 reset, 1 reboot, 2 pairing, 3/4 Wi-Fi, 5 wake-up, 6 eco, 7 power, **8 = 0x74 UP, 9 = 0x75 DOWN, 10 = 0x78 OK**, 11/12 reserved.
3. **`drv_gpio.ko`** creates `/dev/but%d` with minor = that index (`drv_gpio_pin_mgnt`, loop `kSysButtonManaged(0..23)`; the 11 "Button name >= Max(13)" log warnings are indices 13–23).
4. **`libhal_led_but_gpio.so` `HAL_ButtonInit(n)`** opens `/dev/but<n>` with n unchanged (`sprintf` @0x1668).
5. **`libbellosm-smu.so` `CButtonOsm::CButtonOsm`**:
   - `HAL_ButtonInit(10)` + `HAL_ButtonRegister(10, 1, thunk@0x4f4ec → PushedOkBtnEvent)` (@0x4f988/0x4fbe0).
   - `HAL_ButtonInit(8)` + `Register(8, 1, thunk@0x4f56c → PushedUpBtnEvent)` (@0x4fc40/0x4fd00).
   - `HAL_ButtonInit(9)` + `Register(9, 1, thunk@0x4f5e8 → PushedDownBtnEvent)` (@0x4fd60/0x4fe20).
6. The LEDs follow the same path: HAL LED index table `bsp_gpio_lkm .data+0x2a0c`: 21 = 0x6a UP white (GPIO5), **22 = 0x6b OK white (GPIO9)**, 23/24 OK red/yellow (GPIO7/8), **25 = 0x6e DOWN white (GPIO6)**, 26–29 power white/red/blue/brightness. `CScreenOsm::setBtnLed` uses 22 for OK and 25 for DOWN (§1.7).

So on shipped firmware, pressing GPIO15 runs the OK action and lights GPIO9. Bell shipped this UI, so a swapped physical mapping would be an obvious field bug. The DT naming most likely comes from the reference/schematic net order (OK LEDs 6/7/8 contiguous in the DT, 7/8/9 in the BSP). The BSP pairing is internally consistent: button 14 goes with LED 6, button 15 with LED 9, in every source. **Final answer: use the BSP mapping. Confidence ~90%**. The remaining risk is a hardware/UI mismatch, which can only be checked on hardware with `gpioget`/`gpioset`.

### 2.2 Stock behaviour [V]
- Polling: `bsp_gpio_timer` every 10 jiffies (HZ=100, so 100 ms) on flag-0x800 pins. `drv_gpio button_timer` produces hold states: press → state 1. Further states fire at 1 s (2), 2 s (3), 3 s (4), 4 s (5), 5 s (6), 7 s (7), **10 s (8)**, 15 s (9) and 20 s (10). Release → state 0.
- RESET (idx 0): state 1 → `PushedResetBtnEvent`, state 0 → `ReleasedResetBtnEvent` (reboot after ≥5 s, per the UI hint), state 8 (10 s) → `restore_default` (factory reset).

### 2.3 OpenWrt DTS
```dts
keys {
	compatible = "gpio-keys-polled";
	poll-interval = <50>;
	up    { label = "up";    linux,code = <KEY_UP>;      gpios = <&gpio0 13 GPIO_ACTIVE_LOW>; };
	down  { label = "down";  linux,code = <KEY_DOWN>;    gpios = <&gpio0 14 GPIO_ACTIVE_LOW>; };
	ok    { label = "ok";    linux,code = <KEY_ENTER>;   gpios = <&gpio0 15 GPIO_ACTIVE_LOW>; };
	reset { label = "reset"; linux,code = <KEY_RESTART>; gpios = <&gpio1 27 GPIO_ACTIVE_LOW>; }; /* GPIO59 */
};
```
U-Boot does **not** mux 13/14/15/59 (they are absent from its `gpio-pins` group). Stock Linux muxes them to func 5 through `bsp_gpio_set_config` [V]. The reset default of these pads is [U], so OpenWrt must set func 5 for them (pinctrl, or `oled_test`-style `/dev/mem` pokes as a stopgap). Possible alternates: 13–15 are also LED-controller outputs (func 4) and 59 is PCM/I2S (func 1/2).

---

## 3. EFM8BB10F8G MCU (I2C-0 0x78)

### 3.1 Kernel side [V]
- `/etc/init.d/register_mcu_button_reboot` (START=29) runs `insmod mcu-core mcu-i2c mcu-keys no_thermal_alert=1` and then `echo f5689-mcu 0x78 > …/i2c-0/new_device`. `mcu-leds.ko` is not shipped.
- The `mcu-core` variant table: `name="f5689-mcu"`, leds `{"green"}`, 2 keys, `irq_gpio=52`, `use_bcm_irq_api=1`, `ext_irq=4`, fw-version length 2, `pwr_cmd=0x31`. MFD cells: `mcu-leds`, `mcu-keys`.
- sysfs `0-0078/`:
  - `fw_version` (cmd 0x01, read 2 → "0.2").
  - `pwr_set` ("off"/"on"/"reboot" → `[0x31, 0/1/2]`).
  - `fw_loading` / `fw_data` (§3.2).
- Transport: write = `[cmd, data…]`. Read = write `[cmd]`, STOP, then read n (two transactions). In bootloader mode reads use `i2c_smbus_read_byte`.
- **`mcu-keys`** keys `{0x80 KEY_STOP, cmd 0x30, 1 byte, thermal}` (skipped by `no_thermal_alert=1`) and `{0xF7, cmd 0x20, 3 bytes}`. Event values: 2 = pressed, 1 = short release, 3 = long press, each followed by 0 and SYN.
- **IRQ path, pass-2 detail:**
  - `devm_gpio_request_one(52, flags 5, "mcu_int")`, `gpiod_set_debounce(50)`, `gpiod_export`.
  - If the line already **reads 0**, the work is queued at probe. The poll helper `FUN_001000c0` also queues work only when `bcm_gpio_get_data(52) == 0`.
  - The generic path uses `devm_request_threaded_irq(..., flags = 2 = IRQF_TRIGGER_FALLING)`. The BCM path uses `map_external_irq(4)` + `BcmHalMapInterruptEx` (ext IRQ 4 = **GIC SPI 109**, `6858_intr.h INTERRUPT_PER_EXT_4`).
  - So **the SoC sees `mcu_int` as active-low**, while the MCU firmware drives P1.0 **high** on an event (`setb P1.0` @0x02a0/0x02ea/0x0426, `clr` on read @0x0585/0x05a9). The board therefore has an inverting (open-drain) stage [I from the polarity mismatch].
  - On stock the GPIO request fails (-517), so **the stock kernel polls the MCU over I2C every 10 ms** (`input-polled`, interval 10) [V log + code].

### 3.2 Firmware file and update procedure [V]
`/lib/firmware/HH4K_EFM8BB10F8G_Test_V02.efm8` (2003 bytes) uses the Silicon Labs AN945 EFM8 bootloader record format (`'$' len cmd payload`):
- `'1'` setup (keys A5 F1)
- `'2'` erase pages 0x000–0x600
- `'3'` write 128-byte blocks (byte 0 = 0xFF first)
- `'4'` verify 0x0000–0x0759, CRC16 0x19BB (checked: `efm8parse.py`)
- `'3'` byte 0 = 0x02
- `'6'` run.

`/etc/init.d/mcu_firmware_update` (START=30) runs the update if `fw_version` ≠ "0.2": `echo 1 > fw_loading` (app cmd 0x03 → writes 0xA5 to IDATA[0] and software-resets, then the driver polls bootloader version cmd `24 00 01` up to 100 x 20 ms), then `cat file > fw_data` (each record; bootloader replies 0x40 ACK, 0x41 RANGE, 0x42 BADID, 0x43 CRC), then `echo 0 > fw_loading`. The bootloader also answers at 0x78. **Do not reflash unless needed.** The procedure is complete enough to reimplement in userspace with `i2ctransfer`.

### 3.3 Firmware (8051, Keil C51) [V disasm `efm8.dis`]
- **Clocks (re-verified):** `CLKSEL(0xA9)=0`, so SYSCLK = 24.5 MHz HFOSC. `CKCON0(0x8E)=0x32` (T1 prescaled, SCA = /48), `TMOD=0x11`, Timer1 reload 0x9C4F (25521 counts at 510 kHz) → **tick = 50.0 ms**.
- SMBus slave `SMB0ADR=0xF0` (0x78), `SMB0ADM=0xFF`, `SMB0CF |= 0x83`. Crossbar `XBR0=0x04` (SMB0) and `XBR2=0x40`, so SDA = P0.0 and SCL = P0.1 [I, crossbar order]. `P1MDOUT=0x1B`, initial `P1=0x7E` (IRQ low, power ON).
- **Commands** (dispatch on the high nibble, table @0x0684):

| Cmd | Dir | Payload | Function |
|---|---|---|---|
| 0x01 | R | `00 02` | firmware version 0.2 |
| 0x02 | W | – | MCU software reset |
| 0x03 | W | – | enter AN945 bootloader |
| 0x10 | W | `[mask bit0, mode]` | green LED P1.3: 0 off, 1 on, 2 blink 250 ms, 3 blink 1 s, 4 blink 100 ms |
| 0x20 | R | `[held, short, long]` (XDATA 0x28, 0x26, 0x27) | button. Clear-on-read; clears IRQ (@0x055c–0x0585) |
| 0x30 | R | `[thermal]` (XDATA 0x21) | thermal latch. Clear-on-read; clears IRQ |
| 0x31 | W | `[0 off, 1 on, 2 reboot]` | power state (XDATA 0x20, pending flag 0x23) |

- **Button** (P0.7, active-low, polled per tick, @0x027e–0x02ef): presses <2 ticks are ignored. A release between 0.1 s and 5 s sets `short` and raises the IRQ. Holding ≥101 ticks (≈5 s) sets `long` and raises the IRQ, and **if the power state is OFF, the long press turns it back ON**.
- **Power** (P1.1 = 1 when ON): `reboot` drops P1.1 for 4 ticks (≈200 ms) and then returns to ON (@0x042f–0x044f). `off` holds it low and slow-blinks the LED.
- **Thermal** (P0.3 active-low, @0x0407): low for 5 consecutive checks while ON → latch + IRQ. **It does not cut power.** The SoC software decides ("BOARD_PROTECT : Reboot Gateway because of temperature").
- **Watchdog:** routine @0x0456 kicks (`WDTCN=0xA5`) during a start-up delay and then disables it (`0xDE, 0xAD` @0x0494).

### 3.4 What happens if the MCU is not serviced? **Nothing** [V]
There is no heartbeat command, no I2C-activity timeout, and the MCU's own watchdog is disabled. P1.1 changes only on cmd 0x31, or on a long press while OFF. An unserviced MCU leaves the board powered, with latched flags and IRQ asserted (harmless). An OpenWrt image with no MCU driver at all is stable [V by code; nothing in the firmware toggles P1.1 autonomously].

### 3.5 OpenWrt approach
- Minimal: nothing.
- Power button daemon: wait for GPIO52 to go low (gpio-cdev edge FALLING, or poll the GPIO level, which is far cheaper than stock's 10 ms I2C poll), then `i2ctransfer -y 0 w1@0x78 0x20 r3@0x78` and map short/long to `/etc/rc.button/power`.
- `i2cset -y 0 0x78 0x31 0x02` = hard power-cycle reboot (can serve as a `restart` handler). `0x31 0x00` = soft off (a long button press brings it back).
- LED: `i2ctransfer -y 0 w3@0x78 0x10 0x01 <mode>`.
- GPIO52 needs pinmux func 5 [I]. As an IRQ, use `IRQ_TYPE_EDGE_FALLING`. Mainline has no 6858 external-IRQ mux driver, so prefer the gpio-cdev or polled route.

---

## 4. GPIO map, GPIO controller, pinmux, LED controller

### 4.1 Complete board pin table [V unless noted]
Sources: **BSP** = `board_info_5689_bell` (decoder `decodebsp.py`, cross-checked against live `/proc/sys/sagem/gpio/maps` in onu.log). **UB** = U-Boot DT pinctrl/hogs. **DT** = Linux stock DT. **BP** = Broadcom boardparms (02-ethernet). **PT** = `pinmux_table_6858.c`.

| GPIO | Function on this board | Dir / polarity | Init | Mux | Source |
|---|---|---|---|---|---|
| 4 | `phy_enable` | out, high = enable | high (U-Boot) | 5 | UB (not in BSP) |
| 5 | LED screen button UP white | out, act-high | off | 5 | BSP, UB |
| 6 | LED screen button **DOWN** white | out, act-high | off | 5 | BSP (UB/DT name it white-ok, see §2.1) |
| 7 | LED screen button OK red | out, act-high | off | 5 | BSP, UB |
| 8 | LED screen button OK yellow | out, act-high | off | 5 | BSP, UB |
| 9 | LED screen button **OK** white | out, act-high | off | 5 | BSP |
| 10 | LED brightness / "led-control" | out, act-high | **on** | 5 | BSP, UB |
| 13 | Button UP | in, act-low, polled | – | 5 | BSP |
| 14 | Button **DOWN** | in, act-low | – | 5 | BSP |
| 15 | Button **OK** | in, act-low | – | 5 | BSP |
| 23/24 | I2C (I2C2 SCL/SDA per BP) | – | – | 1 | BP, PT [I which controller] |
| 25 | OLED D/C | out | – | 5 | DT, UB |
| 31 | `wifi_2.4g_en` | out, act-high | high | 5 | UB (not in BSP) |
| 32 | XGSPON RX DIS (BSP) / `qosa_power` (UB) | out, act-high | on | 5 | BSP, UB, see 03-pon |
| 39/40 | I2C SDA/SCL (per BP) | – | – | 2 | BP, PT [I] |
| 49 | WAN signal detect (SFP/PON) | in | – | – | BP (02) |
| 50 | `wifi_2g4_power_en` | out, act-high | high | 5 | UB |
| 52 | MCU `mcu_int` (ext IRQ 4) | in, **act-low at SoC** | – | 5 [I] | mcu-core/-keys |
| 54 | HSSPI SS5 → OLED CS# | – | – | **2** | DT, UB, PT |
| 55 | Optical module TX power-down | out | – | – | BP (02) |
| 58 | GPON switch enable (BSP act-high off) / `qosa_switch_en` (UB act-low, output asserted, so both = low) | out | low | 5 | BSP, UB |
| 59 | Button reset | in, act-low, polled | – | 5 | BSP |
| 60 | Select GPON/XGS-PON (1 = GPON) | out, act-high | on | 5 | BSP |
| 62/63, 65/66, 68/69, 71/72 | Ethernet LEDs (eth0..3: LED0 = 1G, LED1 = 10/100), active-low | LED-controller outputs, OPTLED 0/1/3/4/6/7/9/10 | HW | **6** | BP, PT (§4.4) |
| 74 | OLED VCC enable | out, act-high | – | 5 | DT, UB |
| 75 | OLED reset | out, act-low | – | 5 | BSP, DT, UB |
| 76 | LED power white | out, act-high | off | 5 | BSP, UB |
| 77 | LED power red | out, act-high | off | 5 | BSP, UB |
| 78 | LED power blue | out, act-high | **on in U-Boot** | 5 | BSP, UB |
| 80 | PHY reset (all) | out, act-low | deasserted | 5 | BSP, UB |
| 82 | Reset FXS 1 | out, act-low | deasserted | 5 | BSP |
| 83 | Wi-Fi RF disable 2.4 GHz | out, act-low | off | 5 | BSP |
| 84 | Wi-Fi RF disable 5 GHz | out, act-low | off | 5 | BSP |
| 85 | Wi-Fi ext port 5 GHz | out, act-low | off | 5 | BSP |
| 88 | `wifi_5gl_en` | out, act-high | high | 5 | UB |
| 89 | `wifi_5gh_en` | out, act-high | high | 5 | UB |
| 102/103/104 | HSSPI SCLK/MOSI/MISO | – | – | 1 | PT |

New in pass 2: GPIO 4, 31, 50, 88 and 89 (U-Boot hogs, Wi-Fi/PHY power enables). OpenWrt should hog them high like U-Boot does; pass this to the 02/06 owners. There is no buzzer, fan, USB power-enable or SATA pin [V absent]. The reference USB PWRON0/PWRFLT0 pins (113/114 func 1) are not in the Bell tables.

### 4.2 GPIO controller [V]
- Registers: `GpioControl` @0xff800500 (`6858_map_part.h`): `GPIODir[8]` 0x00, `GPIOio[8]` 0x20, `PadCtrl` 0x40, `SpiSlaveCfg` 0x44, `TestControl` 0x48, `TestPortBlockEnMSB/LSB` 0x4c/0x50, `TestPortBlockDataMSB/LSB` 0x54/0x58, `TestPortCmd` 0x5c, `DiagReadBack` 0x60.
- `GPIO_NUM_MAX = 117`. U-Boot pinctrl counts **126 pads** (`brcm,pins-count = <0x7e>`, 7 functions, 0–6 used here).
- Direction 1 = output. Matches mainline `bcm6858.dtsi:141-218` (`gpio0..7: brcm,bcm6345-gpio`, `reg = <0x500+4n 4>, <0x520+4n 4>`) and the U-Boot DT (`gpio-controller@0xff8005XX`). Enable gpio0–gpio3.

### 4.3 Pinmux [V]
- `bcm_set_pinmux(pin, f)`: write `0xff800554 = 0`, `0xff800558 = pin | f<<12`, `0xff80055c = 0x21` (LOAD_MUX_REG_CMD). `PINMUX_DATA_SHIFT = 12`. GPIO = function 5.
- **Mainline `pinctrl-bcm4908.c` uses exactly this 4-register block** (EN_LSB 0x0, DATA_MSB 0x4, DATA_LSB 0x8, CMD 0xc; `pin | function << 12`, `CMD_LOAD_MUX_REG`; lines 19-23, 431-441). On 4908 it sits at 0xff800560. On 6858 the same block is at **0xff800550** (`reg = <0xff800550 0x10>`).
- The driver's pin/group/function tables are 4908-specific (86 pins), so a `brcm,bcm6858-pinctrl` needs a 126-pad table built from `pinmux_table_6858.c`. Effort M, mechanical.
- Stopgaps: the stock U-Boot already sets 4, 5–10, 25, 31, 32, 50, 54(f2), 58, 74–78, 80, 88, 89 [V UB DT]. The rest (13–15, 59, 52, 60, 82–85) are set by a small init script via `/dev/mem` (see `oled_test.c pinmux()`) or a pinctrl hog.

### 4.4 Broadcom LED controller @0xff800800 [V]
- Register map (`6858_map_part.h LedControl`, 0x00–0xd8): glbCtrl, mask, hWLedEn 0x08, serialLedShiftSel, flashRateCtrl[4] 0x10, brightCtrl[4] 0x20, powerLedCfg, pledLut, HwPolarity 0xb4, SwData 0xb8, SwPolarity 0xbc, ParallelLedPolarity 0xc0. This is **identical to mainline `leds-bcm63138.c`** (lines 23-50).
- Stock DT `brcm,bcm63xx-leds` (reg 0xe8). U-Boot DT `brcm,bcm6858-leds` (0xe4, disabled). Mainline dtsi `leds@800 brcm,bcm63138-leds` (`bcm6858.dtsi:238-244`).
- **What it drives:** the 8 Ethernet LEDs. Per `pinmux_table_6858.c:143-156`, `bp_usNetLed0/1` for ports 0–3 = GPIO 62/63, 65/66, 68/69, 71/72 at **func 6 + `BP_PINMUX_HWLED` + OPTLED 0,1,3,4,6,7,9,10**. The LPORT LED block (02-ethernet, `0x8013c074+n*0xc`) generates link/speed/activity, which feeds the controller's HW-LED inputs (`bcm_led_impl1.c`: `hWLedEn`, `HwPolarity = 0xc0000000` init for 6858 B0).
- **The front-panel LEDs are not on it:** no Bell BSP entry has the LED-controller flag, and they are plain GPIOs.
- OpenWrt: mainline `leds-bcm63138` supports only software mode (it clears `HW_LED_EN`, lines 246/284). Either mux 62..72 to func 6 and use leds-bcm63138 with the `netdev` trigger, or add HW-mode support (set `hWLedEn` bits and program LPORT LED link/speed encoding). Alternatively mux them as GPIOs (func 5) and use gpio-leds + netdev. Effort S (SW) / M (HW).

Front LED DTS (BSP names):
```dts
leds {
	compatible = "gpio-leds";
	led-up       { label = "white:up";    gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>; };
	led-down     { label = "white:down";  gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>; };
	led-ok-red   { label = "red:ok";      gpios = <&gpio0 7 GPIO_ACTIVE_HIGH>; };
	led-ok-yel   { label = "yellow:ok";   gpios = <&gpio0 8 GPIO_ACTIVE_HIGH>; };
	led-ok-white { label = "white:ok";    gpios = <&gpio0 9 GPIO_ACTIVE_HIGH>; };
	led-pwr-w    { label = "white:power"; gpios = <&gpio2 12 GPIO_ACTIVE_HIGH>; default-state = "on"; }; /* GPIO76 */
	led-pwr-r    { label = "red:power";   gpios = <&gpio2 13 GPIO_ACTIVE_HIGH>; };                       /* GPIO77 */
	led-pwr-b    { label = "blue:power";  gpios = <&gpio2 14 GPIO_ACTIVE_HIGH>; };                       /* GPIO78, on in U-Boot */
};
&gpio0 { led-brightness-hog { gpio-hog; gpios = <10 GPIO_ACTIVE_HIGH>; output-high; }; };
```

---

## 5. Temperature sensors, fan, power management

- **TMP102 x2** [V]: DTS `tmp102@48`/`@49` on i2c@ff85a800.
  - `registertemperaturesensor` re-adds 0x48 and fails with -16.
  - `libbellosm-smu` reads 0x49 raw (reg 0, 12-bit / 16) and drives `temperature_MessageDegradedLV1..3` and the reboot-on-overtemperature.
  - Locations [U].
  - The MCU thermal input P0.3 is plausibly a TMP102 ALERT [I]. `no_thermal_alert=1` ("Pb Hard") suggests it misfires.
  - OpenWrt: mainline `ti,tmp102` x2.
- **Fan:** none [V absence].
- **pwrmngtd/pwrctl:** generic Broadcom PM. Not needed.

---

## 6. USB

- **Blocks** [V `bcm_usb.ko` + `6858_map_part.h`]: USBH @0x8000c000. `usb_ctrl` @**0x8000c200** (setup: bit4 IOC, bit5 IPP, bit25 STRAP_IPP_SEL, bit28–31 OC disable). EHCI0 0x8000c300, OHCI0 0x8000c400, EHCI1 0x8000c500, OHCI1 0x8000c600, xHCI 0x8000d000 (ECIRA at +0x1f98/0x1f9c).
- **IRQs** (`6858_intr.h`, GIC SPI): XHCI 121, OHCI 124, EHCI 125, OHCI1 122, EHCI1 123, USB events 126. **These differ from 4908**, so don't copy bcm4908.dtsi's 72/73/74.
- **Init** [V decompile]:
  1. `pmc_usb_power_up`.
  2. `setup` bit4/bit5 polarity from boardparms PWRFLT/PWRON (and bit25 strap check).
  3. `usb_pm` (+0x34) clear USB_PWRDWN bit31, then XHC soft reset.
  4. USB3 PHY MDIO via `mdio` (+0x14) / `mdio2` (+0x18): block 0x8040 reg 1 |= 0xF, block 0x9040 reg 1 |= 0xF (SSC enable), block 0x8000 reg 0x0F |= 0x200 (pipe reset enable), blocks 0x8080+ofs reg 5 (sigdet).
- **These are exactly mainline `phy-brcm-usb-init.c` `brcmusb_usb3_ssc_enable`/`usb3_enable_pipe_reset`/`sigdet` (lines 519-680), which `brcm,bcm4908-usb-phy` uses through `bcm7445_ops` (`brcm_usb_dvr_init_4908`, line 1062)** [V source]. So `phy-brcm-usb` should drive the 6858 USB_CTRL block [I, strong].
- What is missing upstream: the 6858 dtsi has no USB/PMB nodes, and the PMB/BPCM power-up of the USB island (`bcm-pmb` supports 4908/63138 only) [U whether 6858 PMB device IDs match].
- **Ports** [V logs]: one physical port (xHCI SS port 1 + EHCI0 port 1). EHCI1/OHCI1 are unused.
- **FUSB301** @ i2c-0 0x25 [V]:
  - Device ID 0x12.
  - Stock writes Modes(0x02) = 0x01 (Source only) and Control(0x03): 0x03 → 0x02 (unmask INT) → 0x04 (HOST_CUR = 10b, 1.5 A).
  - No IRQ is used. There is no VBUS switch on the FUSB301, so VBUS is board-switched [U how].
  - OpenWrt: `i2cset -y 0 0x25 0x02 0x01; i2cset -y 0 0x25 0x03 0x04`. Whether the power-on default already attaches as source is [U].
- DTS sketch (6858 numbers):
```dts
usb_phy: usb-phy@8000c200 { compatible = "brcm,bcm4908-usb-phy"; reg = <0x0 0x8000c200 0x0 0x100>; reg-names = "ctrl";
	dr_mode = "host"; brcm,has-xhci; brcm,has-eohci; #phy-cells = <1>; /* power-domains: TODO 6858 PMB */ };
ehci@8000c300 { compatible = "generic-ehci"; reg = <0x0 0x8000c300 0x0 0x100>; interrupts = <GIC_SPI 125 IRQ_TYPE_LEVEL_HIGH>; phys = <&usb_phy PHY_TYPE_USB2>; };
ohci@8000c400 { compatible = "generic-ohci"; reg = <0x0 0x8000c400 0x0 0x100>; interrupts = <GIC_SPI 124 IRQ_TYPE_LEVEL_HIGH>; phys = <&usb_phy PHY_TYPE_USB2>; };
xhci@8000d000 { compatible = "generic-xhci"; reg = <0x0 0x8000d000 0x0 0x8c8>; interrupts = <GIC_SPI 121 IRQ_TYPE_LEVEL_HIGH>; phys = <&usb_phy PHY_TYPE_USB3>; };
```

---

## 7. I2C controllers [V]

- **Instances:** `i2c@ff85a800` is Linux i2c-0, with TMP102 0x48/0x49, M02099 0x4e, FUSB301 0x25 and MCU 0x78. `i2c@ff802100` is i2c-1, with M02180 0x4e (`with-eeprom`). Both are `brcm,bcm63000-i2c`, reg size 0x60, **no interrupts property** (polled). Broadcom's header confusingly calls 0xff85a800 `I2C_2` and 0xff802100 `I2C` (`6858_map_part.h:201, 255, 2505-2506`).
- **Register map** (`6858_map_part.h` `I2CControl`, lines 2454-2503): ChipAddress 0x00, DataIn0..7 0x04–0x20, CntReg 0x24, CtlReg 0x28 (DTF[1:0], DEGLITCH_DISABLE 0x4, DELAY_DISABLE 0x8, SCL_SEL[5:4], INT_ENABLE 0x40, DIV_CLK 0x80), IICEnable 0x2c (ENABLE, INTRP, NO_ACK, NO_STOP, NO_START), DataOut0..7 0x30–0x4c, CtlHiReg 0x50 (DATA_REG_SIZE 0x40), SclParam 0x54. This is **field-for-field mainline `i2c-brcmstb.c` `struct bsc_regs`** (lines 69-78).
- **Stock driver** (vmlinux `bcm63000_i2c_probe` @0xffffffc000469b00, `bcm63000_xfer` @…9d78): reads an optional u32 speed index (default **5**). It writes `CTL = (idx&3)<<4 | (idx>3 ? DIV_CLK : 0)` (idx 5 = 0x90 = **97.5 kHz**) and **CTLHI = 0x40 (32-bit data registers)**, then accesses 0x24/0x28/0x2c/0x30+/0x04+ with 8-register chunks.
- **Mainline:** `i2c-brcmstb` will drive it.
  - `"brcm,brcmper-i2c"` selects 8-bit data registers (`data_regsz = 1`, lines 676-680) and is exactly what mainline `bcm4908.dtsi` uses for the same IP at 0xff802100 with `clock-frequency = <97500>`.
  - `"brcm,brcmstb-i2c"` selects 32-bit registers like stock. Either works because the driver programs CTLHI.
  - `clock-frequency` must exactly match a table entry (375000, 390000, 187500, 200000, 93750, **97500**, 46875, 50000; lines 111-150, 551-563). Without it the driver defaults to **375 kHz**, which is faster than stock. Use 97500 to stay safe with the EFM8 SMBus slave.
  - With no `interrupts`, it runs in polling mode. The 6858 PERF I2C IRQ is GIC SPI 83 (`INTERRUPT_PER_I2C_IRQ`) [V number, I which instance].
```dts
i2c0: i2c@ff85a800 { compatible = "brcm,brcmper-i2c"; reg = <0x0 0xff85a800 0x0 0x58>; clock-frequency = <97500>;
	#address-cells = <1>; #size-cells = <0>;
	sensor@48 { compatible = "ti,tmp102"; reg = <0x48>; };  sensor@49 { compatible = "ti,tmp102"; reg = <0x49>; };
	/* 0x25 FUSB301 (userspace), 0x4e M02099 (03-pon), 0x78 EFM8 MCU (§3) */ };
i2c1: i2c@ff802100 { compatible = "brcm,brcmper-i2c"; reg = <0x0 0xff802100 0x0 0x58>; clock-frequency = <97500>;
	#address-cells = <1>; #size-cells = <0>; /* 0x4e M02180 + EEPROM (03-pon) */ };
```
Pin muxing of the I2C pads (39/40 func 2, 23/24 func 1 per boardparms) is presumably done by CFE or U-Boot already [U].

---

## 8. Watchdog

- **Hardware** [V]: `6858_map_part.h` puts the TIMR block at 0xff802700 (64-bit timers, `ResetStatus`/`ResetReason`), **WDTIMR0 at 0xff802780** and **WDTIMR1 at 0xff8027c0**. Struct `WDTimer` (lines 1632-1645): `WatchDogDefCount` 0x0, `WatchDogCtl` 0x4 (`0xff00,0x00ff` start; `0xee00,0x00ee` stop), `WDResetCount` 0x8 (reset-pulse length in 50 MHz ticks), `SoftRst` 0xc, `WDAccessCtl` 0x10. IRQs are GIC SPI 14 and 16.
- **Stock** [V `hard_watchdog.ko`]: `count = ms * 50000` written to `0xff802780`, start/stop via `0xff802784`. `HardwareWatchdogInitialize(85000, 5000)`, max_try 60. A kernel timer pacifies every 5 s, but only while userspace keeps refreshing `/proc/watchdog`. Otherwise the board resets ~85 s later. 85 s is the hardware maximum (2^32 / 50 MHz = 85.9 s).
- **U-Boot arms it before Linux** [V strings + upstream source, I timeout]:
  - The Sagemcom U-Boot 2019.07 contains `wdt_bcm6345`, `wdt-reboot`, "WDT:   Started with%s servicing (%ds timeout)". Its DT has `watchdog@ff802780` (first WDT device, `clocks = <refclk50mhz>`, **no `timeout-sec`**).
  - In U-Boot v2019.07 `include/wdt.h:115-143`, `initr_watchdog()` (called from `common/board_r.c:701` when `CONFIG_WDT` is set) **unconditionally calls `wdt_start()`** on the first WDT, with `timeout-sec` or `CONFIG_WATCHDOG_TIMEOUT_MSECS` (default 60 000).
  - U-Boot services it while it runs, but `bootm` does not stop it. **So Linux starts with WDT0 running, probably at 60 s.** Stock survives because `hard_watchdog.ko` takes over from `/etc/modules.d/33-watchdog`.
  - **For OpenWrt the watchdog driver must be built in and procd must ping it, or the board resets about a minute after U-Boot**, which would look like a mysterious boot loop. Confirm on a serial console ("WDT: Started …") [U].
- External reset sources: the MCU's P1.1 power cycle (only on command; §3.4) and the SoC WDT. Nothing else, and no GPIO-kicked external watchdog [V].
- **Mainline:**
  - `bcm7038_wdt.c` (`brcm,bcm6345-wdt`, line 207) uses TIMEOUT 0x0, CMD 0x4 and the same magic values, so it matches.
  - It needs `clocks` = 50 MHz, otherwise it assumes **27 MHz** (`WDT_DEFAULT_RATE`, lines 28, 152-161) and every timeout is off by 1.85x.
  - **Mainline `bcm6858.dtsi:121-137` places `twd: timer-mfd@400` + `watchdog@28` = 0xff800428. That is the BCM4908 layout (4908 TWD at 0xff800400) and is wrong for 6858**, where PERF+0x400 is not the timer block [V 6858 map; I that the dtsi is a copy-paste error]. Override it in the board DTS:
```dts
&twd { status = "disabled"; };                 /* wrong address on 6858 */
/ { periph_clk50: clock-50mhz { compatible = "fixed-clock"; #clock-cells = <0>; clock-frequency = <50000000>; }; };
&{/bus@ff800000} {
	watchdog@2780 { compatible = "brcm,bcm6345-wdt"; reg = <0x2780 0x14>; clocks = <&periph_clk50>; timeout-sec = <30>; };
};
```
  - Reboot: the WDT (soft-reset register `SoftRst` 0xc is also available) or the MCU `0x31 0x02` hard power cycle.

---

## 9. Voice / FXS (brief) [V logs]
`[SLICSLAC] VOICE: voice board [F@ST_5689] daughter board [SI32282_CB]`. Silicon Labs ProSLIC (Si3228x), 2 PCM channels → 2 FXS lines [I]. Control over HSSPI (CS from boardparms), reset FXS1 = GPIO82 (active-low). No mainline support; hold GPIO82 low.

## 10. Other modules noted
`chipinfo.ko` (generic), `sgcnet.ko` (Sagemcom net HAL), `drv_gpio.ko` (`/dev/led`, `/dev/but%d` = BSP button index §2.1), `bsp_gpio_lkm.ko` (`/proc/sys/sagem/{gpio,led,but,relay}/maps`, permanent-parameters reader; relevant to other docs).

## 11. Combined proposed DTS fragment (board level, on top of mainline `bcm6858.dtsi`)
```dts
&gpio0 { status = "okay"; };  &gpio1 { status = "okay"; };  &gpio2 { status = "okay"; };  &gpio3 { status = "okay"; };
&gpio0 { led-brightness-hog { gpio-hog; gpios = <10 GPIO_ACTIVE_HIGH>; output-high; };
         phy-enable-hog     { gpio-hog; gpios = <4 GPIO_ACTIVE_HIGH>;  output-high; };
         wifi24-en-hog      { gpio-hog; gpios = <31 GPIO_ACTIVE_HIGH>; output-high; }; };
&gpio1 { wifi24-pwr-hog     { gpio-hog; gpios = <18 GPIO_ACTIVE_HIGH>; output-high; }; };   /* GPIO50 */
&gpio2 { wifi5l-en-hog      { gpio-hog; gpios = <24 GPIO_ACTIVE_HIGH>; output-high; };      /* GPIO88 */
         wifi5h-en-hog      { gpio-hog; gpios = <25 GPIO_ACTIVE_HIGH>; output-high; }; };   /* GPIO89 */
&hsspi { status = "okay"; };      /* spi-bcm63xx-hsspi; display@5 from §1.6 */
```
Plus the `keys` (§2.3), `leds` (§4.4), `i2c0/i2c1` (§7), USB (§6) and watchdog (§8) nodes.

## 12. Open questions / hardware tests needed
1. Physically confirm GPIO14 = DOWN / GPIO15 = OK (`gpioget`) and LED6/LED9 (`gpioset`). Expected: BSP mapping (§2.1).
2. Serial console: does U-Boot print "WDT: Started with servicing (60s timeout)"? If so, the OpenWrt WDT driver must be built in.
3. Reset-default pinmux of GPIO 13–15/59/52 (are they GPIO without explicit muxing?).
4. What MCU P1.1 switches (expect a full power cycle on `i2cset -y 0 0x78 0x31 2`), and which physical button is on MCU P0.7.
5. Does the FUSB301 attach as a source without the init writes? How is VBUS switched?
6. 6858 USB power domain (PMB) under mainline; `phy-brcm-usb` on 0x8000c200.
7. `oled_test --keep --pattern` on hardware: confirm the border sits at the glass edges (col offset 0x28) and the checker shows no nibble swap.
8. Panel physical size (for DRM `width_mm/height_mm`) and the bell-screend inactivity timeout.

---

## Revision log (pass 2, 2026-11-04)
- **Buttons/LEDs resolved:** traced BSP → `bsp_gpio_lkm` index tables (.data+0x2548 buttons, +0x2a0c LEDs) → `/dev/butN` → `HAL_ButtonInit` → `CButtonOsm` callbacks and `CScreenOsm::setBtnLed`. GPIO14 = DOWN, 15 = OK, LED6 = DOWN white, LED9 = OK white (~90%). Found a third source (U-Boot DT) that agrees with the dead Linux DT; explained why it doesn't matter. Added the reset-button hold timing (10 s = factory reset).
- **OLED:**
  - Found the U-Boot SSD1322 driver and DT (explains `keep`; U-Boot muxes 25/74/75/54).
  - Re-extracted the init bytes and checked them against the datasheet. **Corrected** A0 0x04 (COM scan normal, nibble remap; not "COM reversed") and noted CA = 0x2A is a constant.
  - **Corrected** the HSSPI driver: `spi-bcm63xx-hsspi`, not `spi-bcmbca-hsspi`.
  - Mainline ssd130x incompatibility is now **[V] with file/line citations** (6 independent faults, including missing 0x5C and swapped nibbles).
  - Added `oled_test.c` (builds for aarch64) and the DRM driver sketch, plus the ssd130x patch outline.
- **bell-screend:** screens, data sources, font (OpenSans SemiBold 10/14/30), the asset subset that fits 167 px, `loading.raw` = 168x42 frame, LED enum map.
- **MCU:** verified clock config (50 ms tick), byte order of cmd 0x20, thermal path (latch only), WDT disabled, no heartbeat ("not serviced = nothing happens"). New: IRQ is **active-low at the SoC** (falling edge, ext IRQ 4 = GIC SPI 109), inverted versus the MCU pin. Stock polls I2C every 10 ms.
- **Watchdog:** WDT0/WDT1 addresses and struct from the GPL header. **New: U-Boot 2019.07 starts WDT0 (likely 60 s) before Linux.** **Mainline bcm6858.dtsi watchdog@ff800428 is wrong** (4908 copy). bcm7038_wdt needs a 50 MHz clock.
- **GPIO/pinmux:** full per-pin table with mux functions. New pins 4/31/50/88/89 from U-Boot hogs. Pinmux block = 0xff800550, register-identical to `pinctrl-bcm4908`. 126 pads / 117 GPIOs.
- **I2C:** new §7. Register map identical to `i2c-brcmstb`; stock speed 97.5 kHz and 32-bit data mode; DT props; second controller 0xff802100.
- **USB:** confirmed that the stock MDIO fixups equal mainline `phy-brcm-usb` 4908 ops. Added 6858 IRQ numbers and control-register bits. PMB is still [U].
- **LED controller:** it drives the 8 Ethernet LEDs (func 6, HW-LED/OPTLED map), not the front LEDs. Register map = leds-bcm63138 (SW mode only).
- Pass-1 file saved as `work/periph/04-peripherals.pass1.md`.

## Voice reuse update (2026-11-11)

The earlier unsupported/hold-in-reset voice recommendation concerns a
mainline-kernel port. The installed system retains the stock kernel, and
the backup contains a substantial reusable voice HAL and SIP stack. See
[44-rj11-voice-feasibility.md](44-rj11-voice-feasibility.md) for the verified
assets and the untested RAM bring-up path. Voice remains inactive; no
mainline support or working analog call has been demonstrated.
