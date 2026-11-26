
from hhpanel import Panel
import time
import socket

# UP/DOWN: change pages
# MAIN: return home
# OK: unused, LED always off
#
# MAIN LED:
# Blue = internet reachable
# Flashing red = internet unavailable

PAGES = 3
REFRESH = 1.0
CHECK_INTERVAL = 10.0


def check_internet():
    """Check internet without requiring IDNA."""
    targets = [
        (b"1.1.1.1", 443),
        (b"8.8.8.8", 53),
    ]

    for host, port in targets:
        try:
            with socket.create_connection(
                (host, port), timeout=1.0
            ):
                return True
        except (OSError, LookupError):
            continue

    return False


with Panel() as panel:
    panel.screen(True)

    page = 0
    internet_ok = False
    last_draw = 0
    last_check = -CHECK_INTERVAL

    while True:
        dirty = False
        now = time.monotonic()

        # Handle buttons
        for event in panel.events():
            if event["gesture"] != "tap":
                continue

            button = event["button"]

            if button == "up":
                page = (page - 1) % PAGES
                dirty = True

            elif button == "down":
                page = (page + 1) % PAGES
                dirty = True

            elif button == "main":
                page = 0
                dirty = True

        # Check internet every 10 seconds
        if now - last_check >= CHECK_INTERVAL:
            status = check_internet()

            if status != internet_ok:
                internet_ok = status
                dirty = True

            last_check = time.monotonic()

        # Update display
        if dirty or now - last_draw >= REFRESH:
            panel.clear()

            headings = ("HOME", "NETWORK", "SYSTEM")

            panel.text(
                0, 0,
                headings[page],
                font="small"
            )

            panel.text(
                144, 0,
                f"{page + 1}/{PAGES}",
                font="small"
            )

            panel.rectangle(
                0, 10, 168, 1, color=128
            )

            if page == 0:
                # HOME
                panel.text(
                    0, 12,
                    "{hostname}",
                    font="small"
                )

                status = (
                    "Internet ONLINE"
                    if internet_ok
                    else "Internet OFFLINE"
                )

                panel.text(
                    0, 23,
                    status,
                    font="small"
                )

                panel.text(
                    0, 34,
                    "{date} {time}",
                    font="small"
                )

            elif page == 1:
                # NETWORK
                panel.text(
                    0, 12,
                    "WAN {wan|waiting}",
                    font="small"
                )

                panel.text(
                    0, 23,
                    "LAN {lan|waiting}",
                    font="small"
                )

                panel.text(
                    0, 34,
                    "{wifi}",
                    font="small"
                )

            else:
                # SYSTEM
                panel.text(
                    0, 12,
                    "Up {uptime}",
                    font="small"
                )

                panel.text(
                    0, 23,
                    "Load {load}",
                    font="small"
                )

                panel.text(
                    0, 34,
                    "RAM {memory}%",
                    font="small"
                )

            # LEDs
            panel.led(
                "main",
                "blue" if internet_ok else "red:blink"
            )

            panel.led("up", "white")
            panel.led("down", "white")
            panel.led("ok", "off")

            panel.show()
            last_draw = time.monotonic()

        time.sleep(0.1)

