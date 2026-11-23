"""HH4000 OLED/LED/button API. Use hh-panel-python, or add this directory to PYTHONPATH."""
import json
import os
import re
import socket
import time

class Panel:
    width, height = 168, 42
    def __init__(self, runtime=None):
        self.runtime = runtime or os.environ.get("HH_PANEL_RUNTIME_DIR", "/var/run/hh-frontpanel")
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._socket.settimeout(0.25)
        self._ops = []
        self._seq = 0
        self._instance = None
        self._event = 0
        end = time.monotonic()+3
        while True:
            try:
                current = self.state();break
            except (FileNotFoundError,json.JSONDecodeError):
                if time.monotonic()>end:raise
                time.sleep(.03)
        self._instance = current.get("instance")
        self._event = current.get("events", 0)

    def state(self):
        end = time.monotonic()+10
        while True:
            try:
                with open(os.path.join(self.runtime, "state.json"), encoding="ascii") as f:
                    return json.load(f)
            except (FileNotFoundError,json.JSONDecodeError):
                if time.monotonic()>=end:raise RuntimeError("Panel state unavailable for 10 seconds")
                time.sleep(.05)

    @property
    def variables(self):
        return self.state().get("variables", {})

    def var(self, name, default=""):
        return self.variables.get(name) or default

    def events(self):
        current = self.state()
        if current.get("instance") != self._instance:
            self._instance = current.get("instance")
            self._event = 0
        result = [e for e in current.get("event_log", []) if e["id"] > self._event]
        self._event = current.get("events", 0)
        return result

    @staticmethod
    def _text(value, limit=256):
        value = str(value)
        if any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError("Panel text must not contain control characters or newlines")
        value = value.encode("ascii", "replace").decode("ascii")
        if len(value) > limit:
            raise ValueError("Panel field exceeds %d characters" % limit)
        return value

    @staticmethod
    def _int(value, low, high):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise ValueError("Expected integer from %d to %d" % (low, high))
        return value

    def clear(self):
        self._ops = []
        return self

    def _add(self, *fields):
        if len(self._ops) >= 48:
            raise ValueError("A frame supports at most 48 drawing/LED operations")
        self._ops.append("\t".join(map(str, fields)))
        return self

    def text(self, x, y, text, font="normal", align="left", color=255):
        fonts = {"small": 0, "normal": 1, "large": 2}
        if font not in fonts or align not in ("left", "center", "right"):
            raise ValueError("Invalid font or alignment")
        return self._add("T", self._int(x,-512,512), self._int(y,-512,512), fonts[font], align, self._int(color,0,255), self._text(text))

    def write(self, line1="", line2=""):
        """Replace the frame with two centered lines; templates expand on the router."""
        return self.clear().text(84,2,line1,align="center").text(84,24,line2,align="center")

    def pixel(self, x, y, color=255):
        return self._add("P", self._int(x,-512,512), self._int(y,-512,512), self._int(color,0,255))

    def line(self, x1, y1, x2, y2, color=255):
        return self._add("L", *[self._int(v,-512,512) for v in (x1,y1,x2,y2)], self._int(color,0,255))

    def rectangle(self, x, y, width, height, color=255, fill=False):
        return self._add("R", self._int(x,-512,512), self._int(y,-512,512), self._int(width,1,512), self._int(height,1,512), self._int(color,0,255), int(bool(fill)))

    def led(self, name, value):
        if name not in ("main","up","ok","down"):
            raise ValueError("Unknown LED")
        return self._add("LED", name, self._text(value,79))

    def _send(self, data, require_python=False):
        # A renderer restart must not kill an otherwise healthy panel program.
        # Bounded transport retry does not change modes or restart the program.
        end = time.monotonic()+10
        while True:
            if require_python and self.state().get("settings",{}).get("mode") != "python":
                raise RuntimeError("Select Python mode in LuCI before sending frames")
            try:
                self._socket.sendto(data, os.path.join(self.runtime,"control"))
                return
            except (FileNotFoundError,ConnectionRefusedError,BlockingIOError,socket.timeout):
                if time.monotonic()>=end:raise RuntimeError("Panel control unavailable for 10 seconds")
                time.sleep(.05)

    def _packet(self, kind, records):
        if self.state().get("settings",{}).get("mode") != "python":
            raise RuntimeError("Select Python mode in LuCI before sending frames")
        self._seq += 1
        ident = "p%d-%d" % (os.getpid(),self._seq)
        data = (kind+"\t"+ident+"\n"+"\n".join(records)+"\nEND\n").encode("ascii")
        if len(data)>8192:
            raise ValueError("Frame exceeds 8192 bytes")
        self._send(data, require_python=True)
        return ident

    def show(self, wait=False):
        """Replace the Python-mode frame. It remains displayed until changed or the mode changes."""
        ident = self._packet("FRAME", self._ops)
        if wait:
            end = time.monotonic()+2
            while time.monotonic()<end:
                current = self.state()
                if current.get("last_frame")==ident:
                    return ident
                time.sleep(.03)
            raise RuntimeError(current.get("api_error") or "Panel did not acknowledge the frame")
        return ident

    def publish(self, values):
        """Publish custom variables as {app.name}, without taking over the display."""
        if len(values)>16:
            raise ValueError("At most 16 custom variables")
        records=[]
        for name,value in values.items():
            key = name if name.startswith("app.") else "app."+name
            if not re.fullmatch(r"app\.[A-Za-z0-9_.-]{1,35}",key):
                raise ValueError("Invalid custom variable name")
            value = "1" if value is True else "0" if value is False else "" if value is None else value
            records.append("V\t"+key+"\t"+self._text(value))
        return self._packet("VARS", records)

    def control(self, name):
        allowed = {"home","lan","wifi","system","message","up","down","ok","main","reset","wake","blank","next","previous","menu","select","release","sleep"}
        if name not in allowed and not re.fullmatch(r"(?:page:|hold:)[A-Za-z0-9_.-]{1,39}",name):
            raise ValueError("Invalid panel control")
        self._send(name.encode("ascii"))

    def screen(self, on=True):
        self.control("wake" if on else "sleep")

    def close(self):
        if self._socket is not None:
            self._socket.close()
            self._socket=None

    def __enter__(self):return self
    def __exit__(self,*args):self.close()
