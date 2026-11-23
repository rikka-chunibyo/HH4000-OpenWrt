# Stock display library and reuse investigation

Investigated 2026-11-08 using the backed-up filesystem, followed by an authorized RAM-only test on the actual router. Text, PNG rendering and grayscale rectangles were verified through a USB webcam on the Windows laptop. No firmware, bootloader or persistent configuration was installed for the display test.

## Finding

Sagemcom did create a reusable display toolkit: **`libScreenApi.so.1.0.0`**. It is a small ARM32 C++ widget library built on SDL 1.2, SDL_image and SDL_ttf. Bell's application is a separate executable, `bell-screend`. The library has enough functionality to build a replacement menu UI without reimplementing its drawing primitives.

The renderer does not require Bell's screen-manager messaging interface in its own code. Reusing the binary still requires its C++ ABI, recovered class declarations, and runtime dependencies. The stock-OS route is now demonstrated by a compiled ARM32 test using the real library and panel. A complete replacement application and the higher-level widgets remain unfinished.

## Files preserved in the backup

Paths below are relative to `work/rootfs/`:

| File | Purpose |
|---|---|
| `usr/lib/libScreenApi.so.1.0.0` | Renderer and widget toolkit, 46,376 bytes; aliases `.so` and `.so.1` |
| `usr/bin/bell-screend` | Bell-specific screens, menus and event handling, 157,076 bytes |
| `usr/lib/libIntfScreenmanager.so.1.0.0` | ISM request/event serialization and routing interface, 116,740 bytes |
| `usr/lib/libSDL-1.2.so.0.11.4` | SDL video/surface layer |
| `usr/lib/libSDL_image-1.2.so.0.8.4` | Image loading |
| `usr/lib/libSDL_ttf-2.0.so.0.10.1` | Font rendering, using FreeType |
| `usr/share/screend/fonts/` | OpenSans font files, including the referenced `OpenSans-SemiBold.ttf` |
| `usr/share/screend/images/` | Menu skins, icons and animation frames; some assets target other panel sizes |

`libScreenApi` SHA256:
`94db60a85611c1e2ae16a0879018ed05e7b26816c561e893bf54bd88e89d2cbb`

`libIntfScreenmanager` SHA256:
`7a77c9b79b968b475cc1a1900eddfa80957ee0b20393427ae3a6976fc9988fbb`

No original display SDK headers were found in the inspected filesystem or project source locations.

## The API they exposed

These are actual demangled exports. They establish method names and argument types; exports do not establish return types, complete class layouts, ownership rules or inline setters.

| Class | Useful exported operations |
|---|---|
| `screen::System` | `Instance()`, `start()`, `stop()`, `loadView(View*)`, `refresh(float)`, `genSurfaceFor(Size const&)` |
| `screen::Painter` | `drawFilledRect(Rect const&)`, `drawText(char const*, Point const&)`, `drawImage(char const*, Point const&)`, two `drawSdlSurface(...)` overloads |
| `screen::Font` | Constructor and `set(std::__cxx11::string const&, int)`, `sizeOf(char const*)`, `reloadTTFObj()` |
| `screen::View` | Parent/child view tree, bounding rectangles, `paint(Painter&)`, `animate(float)`, update requests |
| `screen::TextArea` | Text painting, animation, string measurement and bounding-box helpers |
| `screen::ImageArea` | Image loading, surface updates, painting and bounding rectangles |
| `screen::VMenu` | `nextItem()`, `prevItem()`, `trigItem()`, `updateView()`, painting |
| `screen::VYNMenu` | Yes/no menu variant with the same navigation operations |
| `screen::ButtonItem` | Menu association, `focus(bool)`, `trigger()`, `update()` |
| `screen::LoadBar` | Progress/loading bar creation and animation |
| `screen::LoadingArea` | Loading animation creation and animation |
| `screen::TimerArea` | `startTimer()`, time formatting and animation |
| `screen::ScrollArea` | Scroll-area painting |

Full exports: demangled and mangled. Bell's executable imports these classes; they are used by the shipped application.

## Rendering behavior recovered from code

The pipeline is:

`bell-screend views -> libScreenApi widgets/Painter -> SDL surfaces -> SDL fbcon -> /dev/fb0 -> ssd1322.ko -> panel`

`System::start()` at ELF virtual offset `0x56f0` initializes SDL with flags `0x21` (video and timer), initializes SDL_ttf and PNG support, hides the cursor, and requests an 8-bit video surface with `SDL_HWPALETTE`. It sets a 256-entry grayscale palette and creates a separate drawing surface. `refresh(float)` animates the root view, checks its update flag, paints when needed, then blits and calls `SDL_Flip`.

The library's constructor defaults to **136 x 64**, not this router's size. Bell explicitly sets **167 x 42, 8 bpp** before startup. It also sets the global style font size to 12, while individual text widgets can override that. The geometry setup is saved in bell-geometry-setup.asm.

The stock kernel framebuffer rounds the width to **168**, giving a **168 x 42 x 1-byte = 7,056-byte** backing image. The application uses 167 visible columns. A direct framebuffer backend must honor the queried stride; it should not assume the SDL application width is the byte stride. The kernel driver handles the panel's 4-bit grayscale packing.

`Painter` adds its bounding rectangle's x/y origin to drawing coordinates. Text uses `TTF_RenderUTF8_Solid`, blits the temporary text surface and frees it. Without an active font pointer, `drawText` returns without drawing. Images are loaded with `IMG_Load`, converted to the destination format, blitted and freed. Neither a loaded image nor a temporary glyph surface should be assumed to be retained by these immediate drawing calls.

The stock SDL binary contains the fbcon backend and `SDL_FBDEV` / `SDL_VIDEODRIVER` configuration strings. SDL documents these selectors in its [SDL 1.2 environment reference](https://www.libsdl.org/release/SDL-1.2.15/docs/html/sdlenvvars.html). An alternate headless backend has not been demonstrated for this particular binary.

## Binary reuse requirements

The toolkit is ARM32, ARMv7/Cortex-A9, with glibc and C++ `std::__cxx11::string` symbols. The router kernel being AArch64 does not make this library loadable into an AArch64 process. A new application needs a compatible ARM32 compiler/sysroot and C++ runtime, or a separate compatible ARM32 helper process.

The recursive `DT_NEEDED` inventory contains **28 SONAMEs, 6,561,844 bytes total including libScreenApi**, with none missing from the backup: dependency-closure.json. This is the recorded dependency graph, not a tested standalone runtime bundle. In particular, the library uses C++ runtime symbols without declaring `libstdc++.so.6` itself; Bell's executable brings that runtime in. A replacement application must provide it too, normally by linking with the matching C++ toolchain.

There are numerous Sagemcom libraries in `DT_NEEDED`, including `libxmt`, `libMsg` and utility libraries. The actual imported vendor functions observed in the renderer are string/list utilities (`xstr_*`, `xlist_*`), not screen-manager requests or router configuration operations. This suggests the dependency list is broader than its direct drawing needs. Removing dependencies or replacing those utilities has not been tested.

screenapi_layout32.h records the recovered fixed-width ARM32 layouts for Point, Rect, Painter and a System prefix. The 48-byte Painter includes the destination surface, bounds, font string/size/font pointer and RGB color. The full System singleton occupies 616 bytes. These are analysis/FFI notes, **not complete C++ SDK headers**. They must not be used as partial C++ object declarations for construction or subclassing. The layout assertions pass a C11 syntax check.

A binary-backed implementation would reconstruct the minimum complete declarations, link a matching ARM32 application or shim, configure the correct geometry, load a root view and call refresh from an event loop. The recovered method names alone are not sufficient to implement arbitrary widget constructors safely. The live test exercises the actual Painter/Font constructors and destructors and the complete System singleton through explicit symbol calls; it does not construct a replacement C++ widget hierarchy.

## Could we ask the existing Bell UI to draw something?

Potentially. `libIntfScreenmanager` is a separate ISM messaging library, not the drawing toolkit. It exposes `intfScreenManager_sendReq_displayMessage` and corresponding receive/dispatch functions. The send function takes four ARM arguments, allocates a `0x62c`-byte request body, copies a structured message and submits it through the generic request machinery. It is not a simple `displayMessage("text")` function.

The recovered serializer has six leading integer fields, a line collection starting at object offset `0x18`, and trailing fields near `0x61c`. Each line is serialized as a NUL-terminated string in an aligned TLV; integers and TLV headers use network byte order. The field semantics, complete caller prototype and required ISM context/routing have not been established. See screen-interface-api.log. No guessed message was sent to the running service.

This route might preserve the existing menu and add custom notification text on the stock OS. It needs more interface recovery before becoming a usable command. It does not remove the vendor service dependencies of `bell-screend` itself.

Buttons are separate: stock `/dev/butN` events pass through `libbellosm-smu`, ISM and Bell's event handler before calling menu navigation. Reusing `libScreenApi` does not automatically give us button input, LED feedback, blanking timers or network status data.

## Practical implementation choice

For an experiment on the existing OS, reusing `libScreenApi` through a small compatible ARM32 program is reasonable. The binary, fonts, assets and declared dependencies are available. Basic text/image drawing on the real SDL backend has now passed; reconstruct more widget classes only as needed. The test must give one process ownership of the display: the replacement and `bell-screend` should not write the same framebuffer concurrently. During the live test Bell's existing renderer was paused with SIGSTOP, then resumed with SIGCONT; the PID remained the same and was confirmed running afterward.

For a UI intended to survive an OS change, prefer our own small API with operations such as clear, text, image, progress, menu selection, present and blank. Its initial stock backend can use `/dev/fb0`; a future backend can use DRM/fbdev or the known SSD1322 SPI path. Keep button events and router status providers outside the renderer. This avoids exposing the recovered vendor C++ layouts to every application.

There is already implementation material in oled_test.c: framebuffer storage, clipped 5x7 ASCII text, test patterns, raw/PGM loading, panel initialization and pixel transfer. Its SPI/GPIO backend expects a newer kernel with GPIO uAPI v2 and an exposed spidev device; it is not a stock-kernel drop-in. It can be factored into a backend rather than rewritten, but would still need proportional/UTF-8 font rendering, higher-level widgets and input integration. The adjacent DRM driver explicitly remains an uncompiled, hardware-untested sketch.

Wi-Fi working is a required OS milestone; PON remains optional. Choosing a portable display API does not depend on solving either network stack first.

## Verification and limits

[tools/screenapi_probe.py](../tools/screenapi_probe.py) loads the original library into Unicorn, applies its ARM relocations, and executes the original ARM drawing instructions with simulated external calls. Six checks passed:

1. Filled-rectangle coordinate translation and RGB mapping.
2. SDL-surface blit coordinate translation.
3. Image load/convert/blit/free call sequence.
4. Text with no font returns without drawing.
5. Text render/blit/free call sequence.
6. System startup requests 167 x 42 x 8-bit mode and initializes palette/surface handling.

Results: probe-results.json. Decompilation: libScreenApi.decompiled.c. Ghidra imports use a `0x10000` image-base adjustment relative to the ELF symbol values.

These six off-device checks simulate SDL, image decoding, font rendering and logging. The later hardware test below independently covers the real dynamic linker, font/image decoders and physical panel. Buttons, complete widget layouts and Bell's ISM service remain outside the tested scope; a finished replacement UI has not been built or installed.


## Live panel test with Windows USB webcam

The follow-up test used the **Logi C615 HD WebCam attached to the Windows laptop**, not a camera on dev. Captures used 1280x720 MJPEG video input to obtain still photographs; audio was not captured.

The executable is built from screenapi_live_test.c. It is an ARM32 C harness that loads the stock C++ runtime globally, loads `libScreenApi`, and resolves the existing mangled functions. It calls the real System singleton constructor, configures 167x42x8, and uses real Painter/Font constructors, font measurement, `drawText`, `drawFilledRect`, and `drawImage`. SDL blits/flips present the drawing surface. This is a live binary-library test, not emulated rendering or a pre-rendered framebuffer image.

The preflight loaded OpenSans SemiBold at size 12 and measured `HH4000 test` as **68 x 17**. The 40-second display test showed:

1. `HH4000 test` and `Stock library OK` with sixteen grayscale bars.
2. `HH4000 test` and `Text + PNG + bars`, with the stock back-arrow PNG at the upper right.

Both phases were physically legible on camera, and the program exited with status 0. The 7,056-byte framebuffer snapshots contain the expected grayscale levels and differ between the text/image phases. The webcam establishes that the correct pixels actually reached the OLED.

Photographs: text and grayscale, text, PNG and grayscale, and stock UI during boot.

### Vendor blanking behavior matters

The first attempt initialized and drew correctly in framebuffer memory but appeared as a solid white rectangle. The test runner had used the usual Linux unblank value 0. This driver's already-recovered `fb_blank` implementation instead maps **0 to SSD1322 all-on (A5), 1..3 to normal display (A6), and 4 to all-off (A4)**; see section 1.4 of the peripheral notes. The corrected test explicitly sends **FBIOBLANK=1 after SDL initialization**. That change produced the photographed text, image and bars. A future stock framebuffer backend must preserve this unusual mapping rather than use generic Linux blanking assumptions.

### ABI details established by the live test

`Painter` is 48 bytes. Its Font subobject starts at offset **0x0c** and stores **font size first at 0x0c**, then its 24-byte C++ string at **0x10**, then the TTF pointer at **0x28**. RGB color starts at **0x2c**. The earlier analysis header incorrectly placed the filename before the size; it has been corrected from the constructor and validated by the successful live font call. Size-only static assertions could not detect that field-order error.

The library still does not declare libstdc++ as a direct dependency. The harness must load it into the global lookup scope and resolve its string constructor there. The test links against the backed-up glibc and libdl, requiring only GLIBC_2.4 symbols, rather than the build host's newer glibc. A project-local ARM compiler and headers were extracted under `work/display/toolchain/`; no host package installation was needed. [build_screenapi_live.py](../tools/build_screenapi_live.py) reproduces the build using build-settings.json.

### Temporary access and restoration

The original OS was booted with the established RAM-only preinit hook to expose a UART root shell. A temporary BR_LAN alias and two host-specific UDP rules allowed transfer from the existing laptop TFTP server. The executable and runner lived under `/tmp`. No saveenv, flash write, firmware install or persistent startup change was performed for this test.

screenapi_live_run.sh pauses the existing Bell renderer, saves the framebuffer, runs the bounded test, restores the pixels and resumes the same renderer. An independent 55-second guard can also resume it. The successful log records `HH_DISPLAY_STOCK_RESUMED`; `bell-screend` PID 1439 was then confirmed in state S, with no test executable left running. The router was then returned to its normal saved boot path, removing the temporary RAM hook, files, alias and rules. The UART transcript confirms the saved bootcmd marker, stock squashfs mount and stock init. The final webcam photograph confirms Bell's UI is active again; it currently reports Error 1201, low fibre signal. This display test did not assess PON or Wi-Fi connectivity.

Machine-readable findings: hardware-results.json. Logs: test, runner, UART session. The live test covers drawing primitives, font/image decoding and SDL framebuffer presentation. It does not yet validate menus, buttons, the View refresh loop, ISM requests or another OS.
