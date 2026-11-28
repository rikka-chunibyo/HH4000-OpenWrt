/*
 * hh4000_bridge.ino
 * -----------------------------------------------------------------------
 * Transparent UART bridge + out-of-band power control for driving a
 * Bell Home Hub 4000 / Sagemcom FAST5689 console from a script (Python,
 * minicom, raw /dev/ttyACMx, whatever) instead of a human at a terminal.
 *
 * Board: Arduino Mega 2560
 *   Serial   (USB)                 <-> host computer (the thing running
 *                                      Python / an AI agent)
 *   Serial1  (pins 18/19 TX1/RX1)  <-> router UART, through a proper
 *                                      level shifter. Never drive the
 *                                      router's UART RX pin directly from
 *                                      a 5V AVR pin.
 *   RELAY_PIN                      -> coil of a normally-closed relay in
 *                                      series with the router's power
 *                                      input. De-energized = NC closed =
 *                                      router POWERED (fail-safe default).
 *
 * DESIGN PHILOSOPHY
 * -----------------------------------------------------------------------
 * This board does NOT try to understand U-Boot, CFE, or Linux. It is a
 * dumb, reliable, byte-exact pipe between the host and the router's UART,
 * plus the one thing a UART pipe can't do on its own: cut and restore
 * power. All the "intelligence" (deciding what to type, waiting for a
 * prompt, retrying, parsing output) belongs on the host side -- a Python
 * script, an AI coding agent, or a human typing into minicom. That keeps
 * this firmware tiny and the protocol stable even as the actual HH4000
 * workflow changes.
 *
 * WIRE PROTOCOL (host <-> this board, both at 115200 8N1 over Serial/USB)
 * -----------------------------------------------------------------------
 * Router -> host direction (Serial1 -> Serial): completely raw. Every
 * byte forwarded to the host is sent unmodified and
 * un-prefixed. `cat /dev/ttyACM0`, `minicom -D /dev/ttyACM0`,
 * or a Python readline() loop all just see the router's real output.
 *
 * Host -> router direction (Serial -> Serial1) is ALSO raw passthrough,
 * byte for byte, with ONE reserved escape sequence for talking to this
 * board itself:
 *
 *     @HH <COMMAND> [args]\n
 *
 * Any time the exact 4 bytes "@HH " appear in what the host sends, this
 * board switches into command mode, buffers text up to the next \n or \r,
 * executes it as a command, and swallows it -- none of it reaches the
 * router. Anything that is NOT this exact sequence goes straight to the
 * router's UART untouched, including a partial near-match, which gets
 * replayed to the router the instant it fails to match (so you can still
 * send a literal "@HH" as console input if you ever need to).
 *
 * This board's own responses/events are always sent to the host prefixed
 * the same way, so a script can tell "control-plane line" from "router
 * output" just by checking for that prefix:
 *
 *     @HH OK <info>      command succeeded
 *     @HH ERR <reason>   command failed / not understood
 *     @HH BOOT <info>    printed once after this sketch starts
 *
 * Commands:
 *     PING                 -> OK PONG hh4000-bridge/1.0 pwr=ON|OFF uptime_ms=<n>
 *     STATUS               -> OK STATUS pwr=ON|OFF uptime_ms=<n>
 *     BAUD [rate]          -> query or set the target UART rate, keeping
 *                             8N1 framing and the USB link at 115200.
 *                             Replies OK BAUD=<rate>. Switch while the
 *                             target is idle. Reset restores 115200.
 *     PWR ON               -> turn router power on (idempotent)
 *     PWR OFF              -> turn router power off (idempotent)
 *     PWR CYCLE [off_ms]   -> power off, wait off_ms (default 2000, capped
 *                             at MAX_CYCLE_OFF_MS), power back on. Blocks
 *                             only while power is OFF -- nothing useful
 *                             can arrive on the router UART during that
 *                             window anyway -- so passthrough resumes the
 *                             instant power is restored and you don't
 *                             lose the first boot bytes.
 *
 * Unknown commands get "ERR unknown command: <text>".
 *
 * IMPORTANT HARDWARE NOTES
 * -----------------------------------------------------------------------
 * 1. D0/D1 on a Mega 2560 ARE the USB-serial link (the "Serial" object
 *    used for the host connection here). Never put the relay there.
 *    This build uses D12, matching the rebuilt hat relay connection.
 * 2. Opening the USB serial port from a host (pyserial, screen, minicom)
 *    usually toggles DTR, which resets a Mega 2560. On reset, RELAY_PIN
 *    is briefly undriven/floating before setup() runs again. Since the
 *    relay is normally-closed (fail-safe = powered), a brief float is
 *    unlikely to cut power, but if you're going to reconnect often,
 *    disable DTR-triggered reset on the host side (the Python helper
 *    does this) or remove the board's auto-reset capacitor if you want
 *    it gone entirely.
 * 3. If your relay module is active-HIGH instead of the common
 *    active-LOW (opto-relay boards are almost all active-LOW), flip
 *    RELAY_ACTIVE_LOW below. Verify once with "@HH PWR OFF" and confirm
 *    the relay actually clicks and the router loses power, not the
 *    other way around.
 *
 * This sketch deliberately does not handle USB-drive file transfers to
 * the router -- there's no way to automate swapping a physical drive.
 * Everything else (console interaction, power cycling) is scriptable.
 */

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Configuration ---------------------------------------------------------
#define PC_BAUD              115200UL
#define DEFAULT_TARGET_BAUD  115200UL

// AVR USART has a 12-bit divider. The core can use normal or double speed.
constexpr unsigned long MIN_TARGET_BAUD = (F_CPU + 65535UL) / 65536UL;
constexpr unsigned long MAX_TARGET_BAUD = F_CPU / 8UL;
unsigned long targetBaud = DEFAULT_TARGET_BAUD;

#define RELAY_PIN            12      // see hardware note 1 above
#define RELAY_ACTIVE_LOW     0      // see hardware note 3 above

#define CMD_PREFIX           "@HH "
#define CMD_PREFIX_LEN       4
#define CMD_BUF_SIZE         96
#define DEFAULT_CYCLE_OFF_MS 2000UL
#define MAX_CYCLE_OFF_MS     60000UL

// ---- Relay / power control --------------------------------------------------
bool routerPowered = true; // true = NC closed = router has power

void writeRelayEnergized(bool energize) {
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PIN, energize ? LOW : HIGH);
#else
  digitalWrite(RELAY_PIN, energize ? HIGH : LOW);
#endif
}

void setRouterPower(bool on) {
  // Router is fed through the relay's NC contact, so "on" = de-energized.
  writeRelayEnergized(!on);
  routerPowered = on;
}

// ---- Replies ----------------------------------------------------------------
void replyf(const char *fmt, ...) {
  char line[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Serial.print(F("@HH "));
  Serial.println(line);
}

// ---- Command implementations --------------------------------------------------
void cmdPing() {
  replyf("OK PONG hh4000-bridge/1.0 pwr=%s uptime_ms=%lu",
         routerPowered ? "ON" : "OFF", millis());
}

void cmdStatus() {
  replyf("OK STATUS pwr=%s uptime_ms=%lu",
         routerPowered ? "ON" : "OFF", millis());
}

void cmdBaud(const char *args) {
  while (*args == ' ') args++;

  if (*args) {
    char *end;
    unsigned long rate = strtoul(args, &end, 10);
    while (*end == ' ') end++;
    if (*args < '0' || *args > '9' || *end ||
        rate < MIN_TARGET_BAUD || rate > MAX_TARGET_BAUD) {
      replyf("ERR BAUD requires a rate from %lu to %lu",
             MIN_TARGET_BAUD, MAX_TARGET_BAUD);
      return;
    }

    if (rate != targetBaud) {
      Serial1.end();
      Serial1.begin(rate, SERIAL_8N1);
      targetBaud = rate;
    }
  }

  replyf("OK BAUD=%lu", targetBaud);
}

void cmdPwr(char *args) {
  while (*args == ' ') args++;

  if (!strncasecmp(args, "ON", 2) && (args[2] == '\0' || args[2] == ' ')) {
    setRouterPower(true);
    replyf("OK PWR=ON");
  } else if (!strncasecmp(args, "OFF", 3) && (args[3] == '\0' || args[3] == ' ')) {
    setRouterPower(false);
    replyf("OK PWR=OFF");
  } else if (!strncasecmp(args, "CYCLE", 5) && (args[5] == '\0' || args[5] == ' ')) {
    char *rest = args + 5;
    while (*rest == ' ') rest++;
    unsigned long offMs = DEFAULT_CYCLE_OFF_MS;
    if (*rest) {
      long v = atol(rest);
      if (v > 0 && (unsigned long)v <= MAX_CYCLE_OFF_MS) offMs = (unsigned long)v;
    }
    setRouterPower(false);
    delay(offMs); // safe to block here: router has no power, nothing to miss
    setRouterPower(true);
    replyf("OK PWR=ON cycled off_ms=%lu", offMs);
  } else {
    replyf("ERR bad args: PWR ON|OFF|CYCLE [off_ms]");
  }
}

void processCommand(char *text) {
  char *sp = strchr(text, ' ');
  size_t wordLen = sp ? (size_t)(sp - text) : strlen(text);

  if (wordLen == 4 && !strncasecmp(text, "PING", 4)) {
    cmdPing();
  } else if (wordLen == 6 && !strncasecmp(text, "STATUS", 6)) {
    cmdStatus();
  } else if (wordLen == 4 && !strncasecmp(text, "BAUD", 4)) {
    cmdBaud(sp ? sp + 1 : "");
  } else if (wordLen == 3 && !strncasecmp(text, "PWR", 3)) {
    cmdPwr(sp ? sp + 1 : (char *)"");
  } else {
    replyf("ERR unknown command: %s", text);
  }
}

// ---- Host byte handling: detect "@HH " and otherwise pass through raw --------
char cmdBuf[CMD_BUF_SIZE];
uint8_t cmdLen = 0;
bool inCommand = false;
bool invalidCommand = false;
bool skipCommandLF = false;
uint8_t prefixMatched = 0;

void handleHostByte(char c) {
  if (skipCommandLF) {
    skipCommandLF = false;
    if (c == '\n') return;
  }

  if (inCommand) {
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = '\0';
      if (invalidCommand) {
        replyf("ERR invalid command frame");
      } else if (cmdLen > 0) {
        processCommand(cmdBuf);
      }
      inCommand = false;
      skipCommandLF = c == '\r';
    } else if (c != '\0' && cmdLen < CMD_BUF_SIZE - 1) {
      cmdBuf[cmdLen++] = c;
    } else {
      invalidCommand = true;
    }
    return;
  }

  const char *prefix = CMD_PREFIX;
  if (c == prefix[prefixMatched]) {
    prefixMatched++;
    if (prefixMatched == CMD_PREFIX_LEN) {
      inCommand = true;
      invalidCommand = false;
      cmdLen = 0;
      prefixMatched = 0;
    }
    return;
  }

  // Mismatch: the bytes we were holding back were real router-bound input.
  for (uint8_t i = 0; i < prefixMatched; i++) Serial1.write(prefix[i]);
  prefixMatched = (c == prefix[0]) ? 1 : 0;
  if (prefixMatched == 0) Serial1.write(c);
}

// ---- Setup / loop -------------------------------------------------------------
void setup() {
  // Set the relay to "router powered" as the very first thing this sketch
  // does, before anything else can stall or branch.
  pinMode(RELAY_PIN, OUTPUT);
  setRouterPower(true);

  Serial.begin(PC_BAUD);
  Serial1.begin(targetBaud, SERIAL_8N1);
  delay(200);
  replyf("BOOT hh4000-bridge/1.0 pwr=ON");
}

void loop() {
  // Keep control commands responsive even if a fast target fills its RX buffer.
  if (Serial1.available() && Serial.availableForWrite()) {
    Serial.write((uint8_t)Serial1.read());
  }
  if (Serial.available()) {
    handleHostByte((char)Serial.read());
  }
}
