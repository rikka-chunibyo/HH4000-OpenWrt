#!/usr/bin/env python3
"""
hh4000_bridge.py
-----------------------------------------------------------------------
Reference client for the hh4000_bridge.ino Arduino sketch: a transparent
UART bridge + relay-based power control for the Bell Home Hub 4000.

Protocol recap (hh4000_bridge.ino is the authoritative description):
  - Everything the router sends arrives on the serial port completely raw.
  - To send a command to the *bridge itself* (not the router), write the
    literal bytes "@HH <COMMAND>\n". The bridge's own replies are lines
    starting with "@HH " too, so they're trivially distinguishable from
    router output.
  - Everything else you write goes straight to the router's UART, byte
    for byte, no translation.

Requires: pyserial (`pip install pyserial`)

Library usage:

    from hh4000_bridge import HH4000Bridge

    with HH4000Bridge("/dev/ttyACM0") as hub:
        hub.power_cycle()
        hub.wait_for("Autoboot in")
        hub.send_raw(b" ")           # interrupt autoboot
        hub.wait_for("=> ")
        hub.send("printenv")
        print(hub.read_for(seconds=1))

CLI usage:

    python3 hh4000_bridge.py /dev/ttyACM0 --cycle
    python3 hh4000_bridge.py /dev/ttyACM0 --send "printenv"
    python3 hh4000_bridge.py /dev/ttyACM0 --monitor
"""

import argparse
import sys
import time

import serial  # pyserial


class HH4000Bridge:
    def __init__(self, port, baud=115200, timeout=0.05):
        # dtr/rts False before open: avoids toggling DTR on connect, which
        # resets a Mega 2560 (and would briefly float the relay pin). If
        # your OS/driver resets it anyway, that's happening below pyserial;
        # see the sketch's hardware notes for the hardware-level fix.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    # -- low-level ------------------------------------------------------
    def _read_available(self):
        n = self.ser.in_waiting
        if n:
            return self.ser.read(n)
        return b""

    def _command(self, text, timeout=5.0):
        """Send a "@HH ..." control command and wait for its "@HH OK/ERR" reply."""
        self.ser.write(b"@HH " + text.encode() + b"\n")
        deadline = time.time() + timeout
        pending = b""
        while time.time() < deadline:
            chunk = self._read_available()
            if chunk:
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    line = line.strip(b"\r")
                    if line.startswith(b"@HH "):
                        return line[4:].decode(errors="replace")
                    # else: router chatter interleaved with our command's
                    # reply -- dropped here; stash it yourself if you need it.
            else:
                time.sleep(0.01)
        raise TimeoutError(f"no @HH reply to: {text}")

    # -- power control ----------------------------------------------------
    def power_on(self):
        return self._command("PWR ON")

    def power_off(self):
        return self._command("PWR OFF")

    def power_cycle(self, off_ms=2000):
        # Timeout covers the off_ms the sketch blocks for, plus margin.
        return self._command(f"PWR CYCLE {off_ms}", timeout=(off_ms / 1000.0) + 5.0)

    def status(self):
        return self._command("STATUS")

    def ping(self):
        return self._command("PING")

    # -- router console I/O ------------------------------------------------
    def send(self, text, newline="\r"):
        """Send raw text to the router's console (not a bridge command)."""
        self.ser.write(text.encode() + newline.encode())

    def send_raw(self, data: bytes):
        self.ser.write(data)

    def read_for(self, seconds=1.0):
        """Collect whatever the router outputs for a fixed window."""
        end = time.time() + seconds
        out = b""
        while time.time() < end:
            out += self._read_available()
            time.sleep(0.01)
        return out.decode(errors="replace")

    def wait_for(self, pattern, timeout=30.0):
        """
        Block until `pattern` (plain substring, or a compiled regex with a
        .search() method) appears in the router's output, or raise
        TimeoutError. Returns everything read up to and including the match.
        """
        is_regex = hasattr(pattern, "search")
        end = time.time() + timeout
        buf = ""
        while time.time() < end:
            chunk = self._read_available()
            if chunk:
                buf += chunk.decode(errors="replace")
                if (is_regex and pattern.search(buf)) or \
                   (not is_regex and pattern in buf):
                    return buf
            else:
                time.sleep(0.01)
        raise TimeoutError(f"pattern not seen within {timeout}s: {pattern!r}")


def _main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cycle", action="store_true", help="power-cycle the router")
    ap.add_argument("--off-ms", type=int, default=2000)
    ap.add_argument("--send", metavar="TEXT", help="send a line to the router console")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--monitor", action="store_true",
                     help="print raw router output until Ctrl-C")
    args = ap.parse_args()

    with HH4000Bridge(args.port, args.baud) as hub:
        if args.status:
            print(hub.status())
        if args.cycle:
            print(hub.power_cycle(args.off_ms))
        if args.send is not None:
            hub.send(args.send)
        if args.monitor or args.send is not None or args.cycle:
            try:
                while True:
                    chunk = hub._read_available()
                    if chunk:
                        sys.stdout.write(chunk.decode(errors="replace"))
                        sys.stdout.flush()
                    else:
                        time.sleep(0.01)
            except KeyboardInterrupt:
                pass


if __name__ == "__main__":
    _main()
