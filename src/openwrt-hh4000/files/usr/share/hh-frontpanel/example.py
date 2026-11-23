# UP/DOWN: switch views. OK: toggle the OK lamp. MAIN: go home.
# Python mode keeps the last frame until changed; switch modes explicitly in LuCI.
from hhpanel import Panel
import time

with Panel() as panel:
    panel.screen(True)
    page = 0
    lamp = True
    while True:
        for event in panel.events():
            if event["gesture"] != "tap":
                continue
            if event["button"] == "up": page = (page - 1) % 3
            elif event["button"] == "down": page = (page + 1) % 3
            elif event["button"] == "main": page = 0
            elif event["button"] == "ok": lamp = not lamp
        if page == 0:
            panel.write("{hostname}", "WAN {wan|waiting}")
        elif page == 1:
            panel.write("LAN {lan|waiting}", "{wifi}")
        else:
            panel.clear()
            panel.text(0, 1, "Up {uptime}", font="small")
            panel.text(0, 12, "Load {load}  RAM {memory}%", font="small")
            panel.text(0, 23, "{date} {time}", font="small")
            panel.rectangle(0, 35, 168, 6, color=128)
        panel.led("main", "{wan_up?white:blue}:blink")
        panel.led("up", "white").led("down", "white")
        panel.led("ok", "amber" if lamp else "off")
        panel.show()
        time.sleep(0.2)
