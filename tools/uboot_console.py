#!/usr/bin/env python3
"""Run explicit UART commands, recording an append-only transcript. No flash actions by default."""
import argparse
import pathlib
import re
import sys
import time
import serial


class Console:
    def __init__(self, port, log):
        self.log = open(log, 'ab')
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, .05
        self.ser.dtr = self.ser.rts = False
        self.ser.open()

    def read(self, seconds):
        end, data = time.monotonic() + seconds, b''
        while time.monotonic() < end:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                data += chunk
                self.log.write(chunk)
                self.log.flush()
        return data

    def command(self, cmd, timeout=30):
        self.read(.2)
        self.ser.write(b'\x15' + cmd.encode() + b'\r')
        data, end = b'', time.monotonic() + timeout
        while time.monotonic() < end:
            data += self.read(.1)
            if re.search(rb'(?:^|[\r\n])=> $', data):
                return data.decode(errors='replace')
        raise TimeoutError(f'{cmd}: {data[-2000:]!r}')

    def close(self):
        self.ser.close()
        self.log.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', default='COM3')
    p.add_argument('--log', default=str(pathlib.Path(__file__).resolve().parents[1] / 'work/boot/live/repair-session.raw'))
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('commands', nargs='+')
    a = p.parse_args()
    c = Console(a.port, a.log)
    try:
        for command in a.commands:
            print(c.command(command, a.timeout), flush=True)
    finally:
        c.close()
