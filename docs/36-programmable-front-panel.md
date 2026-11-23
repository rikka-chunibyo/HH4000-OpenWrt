# Configurable and programmable front panel

## Two explicit modes

Open **System -> Front Panel** in LuCI and choose **Configuration** or **Python** under Mode and hardware.

* **Configuration** runs the editable page list, template text, automatic rotation, screen timeout, configured tap/hold actions and LED rules.
* **Python** gives the saved program control of drawing, button behavior and LEDs. Configuration pages/actions/rotation/timeout/LED rules do not run. The last drawing instructions remain when updates stop, the program exits, Stop is pressed or an error occurs. Template values inside that drawing continue to refresh. There is no automatic return to Configuration. Switch the mode explicitly to do that.

The master enable, screen enable, brightness, hold threshold and read-only status variables are shared hardware facilities. A panel-service restart or router reboot clears the in-memory frame and starts the selected system again; a saved Python program runs at startup if Python mode is selected.

The installed update is left in **Configuration** mode with the existing user settings preserved. Refresh LuCI after the update to load the new page.

## Configuration mode

Add, remove, reorder, enable and edit up to 16 named pages. Each has a menu-label template, font, alignment and up to four text lines. Normal font is roughly 20 characters on two lines, small font about 42 characters on four lines, large font about 10 characters on one line. The physical OLED is 168x42 pixels. Expanded text is clipped to the screen; template input can be up to 256 printable ASCII characters.

Choose the home page and optional automatic rotation interval. Rotation pauses in the menu. Each of UP, DOWN, OK and main has a tap and hold action: previous/next, menu/select, home, wake, blank, no action or a named page. Tap fires on release; hold fires once after the configured threshold and suppresses tap. No action invokes Bell's old hold-to-reboot behavior.

Main, UP, OK and DOWN lights have separate rules. Main supports white/blue/red/off, OK white/amber/red/off, UP/DOWN white/off. Append `:blink`, `:fast`, `:pulse` (double flash) or `:awake`. These are on/off GPIO patterns, not PWM fades. The navigation light policy can follow screen activity, remain enabled or disable the navigation lights. No untested mixed colors are generated.

### Templates

All screen text and menu-label fields expand variables, as do Configuration LED fields:

```text
LAN {lan}
WAN {wan|waiting}
{wan_up?Online:Offline}
{status_color}:blink
```

`{name}` substitutes a value; `{name|fallback}` substitutes the fallback when missing/empty; `{name?yes:no}` chooses using its truth value. Use `{{` and `}}` for literal braces. False values include empty, 0, false, off, down, pending and error. Unknown variables remain visible. Expansion is bounded, does not execute code and supports bounded recursive substitution (for example the home page's `{title}` can itself contain `{hostname}`).

Available built-ins include:

| Variables | Meaning |
| --- | --- |
| `{lan}`, `{wan}`, `{wan6}` | LAN/WAN IPv4 and first WAN IPv6 address; empty when unavailable |
| `{ip}`, `{status}`, `{health}` | Selected status network's IPv4/address readiness: ready, pending or error |
| `{lan_up}`, `{wan_up}`, `{wan6_up}` | 1 or 0 |
| `{lan_status}`, `{wan_status}`, `{wan6_status}` | up, down or error |
| `{status_color}`, `{lan_color}`, `{wan_color}`, `{wan6_color}` | white/blue/red status colors |
| `{hostname}`, `{uptime}`, `{uptime_seconds}` | Hostname, formatted uptime and uptime seconds |
| `{load}`, `{memory}`, `{free_memory}` | One-minute load, used RAM percentage and available RAM in MB |
| `{date}`, `{time}` | Current local date/time |
| `{ports}`, `{eth0}` ... `{eth4}` | Link summary and per-port up/down |
| `{eth0_speed}` ... `{eth4_speed}` | Negotiated speed in Mb/s, or 0 |
| `{wifi}`, `{wifi1}`, `{wifi2}`, `{radio0}` ... `{radio2}` | Radio summaries and up/off states |
| `{page}`, `{button}`, `{screen_on}` | Current page, last button and screen state |
| `{title}`, `{message_line1}`, `{message_line2}` | Shared text fields, allowing nested templates |

Status describes interface/address readiness, not an internet reachability probe. The provider reads only the required network fields and never publishes Wi-Fi credentials.

## Python mode

Select Python, edit the program in the Python program tab, then **Save & Apply**. Examples can be loaded directly into the editor. The program runs with router administrator privileges. The editor accepts up to 32KiB. Larger applications can live in ordinary files and be launched from a short editor program or from the shell.

**Start / restart** runs the saved program and restarts the panel service. **Stop** terminates the program while retaining its last frame and LEDs. Programs that exit are not automatically rerun. Syntax/runtime errors are displayed in LuCI. Programs do not become Configuration mode after an error.

This is a complete one-shot program; its output remains after it exits:

```python
from hhpanel import Panel

with Panel() as panel:
    panel.write("LAN {lan}", "WAN {wan|waiting}")
    panel.led("main", "blue")
    panel.led("up", "white")
    panel.led("ok", "amber")
    panel.led("down", "off")
    panel.show()
```

For interactive programs, consume button events and update at a modest rate:

```python
from hhpanel import Panel
import time

with Panel() as panel:
    count = 0
    while True:
        for event in panel.events():
            if event["button"] == "ok" and event["gesture"] == "tap":
                count += 1
        panel.write("OK presses: " + str(count), "{time}")
        panel.show()
        time.sleep(0.2)
```

Events contain `id`, `button` (up/down/ok/main), `gesture` (press/tap/hold/release), `action` and `source`. Python receives the event; Configuration actions are ignored. The history holds the latest 32 events, so poll regularly. `Panel.events()` returns only new events to that client and handles renderer restarts.

### API

A frame begins with `clear()` (black background); add operations and send it with `show()`. `write()` is a shortcut that clears and creates two centered text lines. Coordinates are pixels from the top-left. Drawing is clipped to the display.

```python
panel.clear()
panel.text(0, 1, "{hostname}", font="small")
panel.text(84, 12, "Custom graphics", align="center")
panel.line(0, 35, 167, 35, color=128)
panel.rectangle(0, 37, 90, 4, fill=True)
panel.pixel(167, 41)
panel.led("main", "{wan_up?white:blue}:blink")
panel.show(wait=True)
```

* `text(x, y, text, font="normal", align="left", color=255)`: fonts small/normal/large; alignment is relative to x. Text templates expand in the renderer.
* `pixel`, `line`, `rectangle`: grayscale color 0-255; the global brightness also applies.
* `led(name, value)`: main/up/ok/down and supported color/pattern/template. LED values persist when later frames omit them.
* `screen(True/False)`: explicitly wake or sleep the screen. Python mode has no Configuration idle timer.
* `show(wait=False)`: replace the Python frame, optionally waiting for acknowledgment. There is no frame timeout.
* `state()`, `variables`, `var(name, default="")`, `events()`: read live state/data and new events.
* `publish({"counter": 42, "online": True})`: add Python-mode variables `{app.counter}` and `{app.online}`. Booleans become 1/0; None becomes empty, so conditional/fallback templates work naturally.
* `control("release")`: explicitly clear the Python frame/variables to the Python waiting screen; this does not select Configuration mode.
* `close()` or leaving `with Panel()`: closes the client socket and retains the last frame.

Configuration mode rejects Python drawing/variable updates. A frame supports up to 48 drawing/LED records and an 8192-byte packet; custom variables are limited to 16. Invalid or truncated frames do not replace the previous frame. The API uses an owner-only local Unix socket, with no network listener.

The API is installed at `/usr/lib/hh-frontpanel/hhpanel.py`. Run standalone files with:

```sh
hh-panel-python /root/my_panel.py
```

LuCI sets the module path automatically. Examples are in `/usr/share/hh-frontpanel/example.py` and `variables-example.py`. Python 3.11 light/urllib support was installed on the current router; additional standard-library/third-party modules can be installed as needed. Minimal future RAM images can use Configuration mode without Python; install `python3-light python3-urllib` to enable the Python runtime there.

## Validation and installation

The renderer/model passes sanitizer-backed checks for templates, page navigation, gestures, clipping, malformed frames and mode ownership. Host integration tests exercise the actual daemon/socket/SDK and program runner, including retained one-shot output, errors, mode changes and Boolean variables. The LuCI view passes a Node smoke test for rendering, preview, examples and validation.

Live RAM tests read the physical framebuffer back and matched the preview exactly. The actual LED GPIO states matched a blue main ring, white UP and amber OK. A camera image confirmed the screen and colors. Live tests verified that Python ignores Configuration timers/actions, retains frames after exit/error, and only returns to Configuration on an explicit mode change. The original panel was restored to verify rollback before installation.

Installed LuCI/UCI tests added a template page, selected it with a configured action, exercised hold, ran a saved one-shot Python program, confirmed no automatic rerun, stopped a running program without losing its frame, reported a syntax error and restored the user's Configuration settings. A WAN traffic check passed during the work. No router reboot or network restart was used for panel testing; rpcd was restarted to load the new fixed-path permissions.

Only panel files, panel configuration and Python package dependencies were written to the data overlay. Bootloaders, boot environment, kernel, read-only base image and network configuration were not modified. Source changes are under `src/openwrt-hh4000/frontpanel/` and panel-specific files under `src/openwrt-hh4000/files/`; build/test/device tools are under `tools/`.

Artifacts are in `work/frontpanel-v2/`: final overlay/manifest, sanitizer and host integration logs/results, live hardware results, LuCI API results, camera image, final results, and `original-live.tar.gz` plus original file hashes. The backup was hash-verified before installation. For rollback, stop the panel, preserve any newer user program/configuration, remove only newly added panel paths absent from the original archive, restore the archive and restart the panel/rpcd. Python dependencies can remain installed. Do not reset/erase the entire data volume to undo the panel update.

Final results: `work/frontpanel-v2/final-results.json`. Validation passed 54 model/sanitizer checks, 25 host daemon/API/program checks, 12 live hardware checks and 7 LuCI/UCI program checks. All 17 installed code/helper paths matched the final manifest; all 14 original main options were preserved. The panel was restarted independently and returned ready in Configuration mode. Python 3.11.16 is available, with about 331MiB data space free. Temporary test services, RAM staging and TFTP copies were removed. The network check returned 98/98 UDP replies during the work.
