#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// Bell HH4000 / Sagemcom FAST5689 - Arduino Mega 2560 UART bridge
// Tested boot layout: 4KOS_20.2.152_prod, BCM6858, U-Boot 2019.07.
// Mega Serial1: TX1=pin 18 -> router RX (through proper voltage interface),
//               RX1=pin 19 <- router TX; common ground.
// WARNING: Never drive an unknown router UART voltage directly from a 5V pin.
//
// ~b: original RAM-only PID 1 root shell (recovery mode)
// ~f: same boot, then RAM-only preinit hook, real BusyBox init + UART root
// Neither path invokes saveenv / NAND programming, but NORMAL router services
// started by ~f MAY modify their own persistent flash configuration.

#define PC_BAUD      115200UL
#define ROUTER_BAUD  115200UL

const char bootTrigger[] = "Autoboot in";
size_t bootMatch = 0;

bool armed = true;
bool escapeMode = false;
bool swallowLocalEOL = false;
bool skipNextLF = false;
bool warnedInput = false;

bool ubootReady = false;
bool atLineStart = true;
uint8_t promptMatch = 0;

bool rootSequence = false;
bool fullRequested = false;
uint8_t rootStep = 0;
bool stepVerified = false;
bool stepError = false;
size_t responseMatch = 0;
size_t errorMatch = 0;
const uint8_t ROOT_STEPS = 9;

enum FullPhase : uint8_t {
  FP_IDLE,
  FP_WAIT_LINUX,
  FP_WAIT_HOOK,
  FP_WAIT_INIT,
  FP_WAIT_SHELL,
  FP_VERIFY,
  FP_COMPLETE,
  FP_FAILED
};
FullPhase fullPhase = FP_IDLE;
unsigned long phaseStarted = 0;
uint8_t hookStep = 0;
const uint8_t HOOK_STEPS = 9;
size_t shellMatch = 0;
size_t okMatch = 0;
size_t noMatch = 0;
size_t loginMatch = 0;
size_t readyMatch = 0;
size_t failedMatch = 0;
char okToken[18];
char noToken[18];

// Nonblocking Linux command transmitter. Process UART RX between TX bytes.
char txLine[240];
uint16_t txLength = 0;
uint16_t txPosition = 0;
bool txActive = false;
unsigned long lastTxMicros = 0;

void setPhase(FullPhase next) {
  fullPhase = next;
  phaseStarted = millis();
  shellMatch = okMatch = noMatch = loginMatch = 0;
  readyMatch = failedMatch = 0;
}

bool matchText(char c, const char *pattern, size_t &pos) {
  if (c == pattern[pos]) ++pos;
  else pos = (c == pattern[0]) ? 1 : 0;
  if (pattern[pos] == '\0') {
    pos = 0;
    return true;
  }
  return false;
}

void showHelp() {
  Serial.println(F("\n=== HH4000 UART Console ==="));
  Serial.println(F("~s  Send SPACE (manual U-Boot interrupt)"));
  Serial.println(F("~a  Arm automatic U-Boot interrupt"));
  Serial.println(F("~d  Disarm automatic U-Boot interrupt"));
  Serial.println(F("~b  RAM-only root shell (recovery)"));
  Serial.println(F("~f  FULL firmware boot + UART root (RAM hook)"));
  Serial.println(F("~x  Cancel running automation"));
  Serial.println(F("~?  Show help"));
  Serial.println(F("~~  Send literal ~"));
  Serial.println(F("Other input passes through to the HH4000."));
  Serial.println(F("===========================\n"));
}

void sendInterrupt() {
  Serial1.write(' ');
  Serial1.flush();
  armed = false;
  ubootReady = false;
  Serial.println(F("\n[HH4000] SPACE sent to U-Boot!"));
}

void checkBootMessage(char c) {
  if (!armed) return;
  if (matchText(c, bootTrigger, bootMatch)) {
    bootMatch = 0;
    sendInterrupt();
  }
}

void failFull(const __FlashStringHelper *reason) {
  txActive = false;
  fullRequested = false;
  setPhase(FP_FAILED);
  Serial.print(F("\n[FULL] STOPPED: "));
  Serial.println(reason);
  Serial.println(F("[FULL] UART passthrough restored; check the router prompt."));
  Serial.println(F("[FULL] If PID 1 is still sh, you can retry manually."));
}

void stopAutomation() {
  if (!rootSequence && (fullPhase == FP_IDLE ||
      fullPhase == FP_COMPLETE || fullPhase == FP_FAILED)) {
    Serial.println(F("\n[HH4000] No active automation."));
    return;
  }
  rootSequence = false;
  fullRequested = false;
  txActive = false;
  setPhase(FP_IDLE);
  Serial.println(F("\n[HH4000] Automation cancelled; UART is manual."));
  Serial.println(F("[HH4000] An interrupted shell command may need Ctrl-C."));
}

const char *expectedResponse(uint8_t step) {
  switch (step) {
    case 1: return "Read 44195296 bytes from volume operational";
    case 2: return "0101f000: 27 05 19 56";
    case 3: return "03a24000: d0 0d fe ed";
    case 7: return "init=/bin/sh";
    default: return nullptr;
  }
}

void checkRootResponse(char c) {
  if (!rootSequence) return;
  const char *wanted = expectedResponse(rootStep);
  if (wanted && !stepVerified && matchText(c, wanted, responseMatch)) {
    stepVerified = true;
  }
  if (rootStep >= 4 && rootStep <= 6 &&
      matchText(c, "Usage:", errorMatch)) {
    stepError = true;
  }
}

void sendRootStep(uint8_t step) {
  rootStep = step;
  responseMatch = errorMatch = 0;
  stepVerified = stepError = false;
  ubootReady = false;

  Serial.print(F("\n[BOOT] U-Boot step "));
  Serial.print(step + 1);
  Serial.print('/');
  Serial.println(ROOT_STEPS);

  switch (step) {
    case 0:
      Serial1.print(F("ubi part ubi"));
      break;
    case 1:
      Serial1.print(F("ubi read 0x01000000 operational"));
      break;
    case 2:
      Serial1.print(F("md.b 0x0101f000 0x4"));
      break;
    case 3:
      Serial1.print(F("md.b 0x03a24000 0x4"));
      break;
    case 4:
      Serial1.print(F("setenv bootargs \"${mtdparts} "
                      "ubi.mtd=ubi,0 part_main=ubi part_boot=boot "
                      "image_ubivol=operational board_type=00030103\""));
      break;
    case 5:
      Serial1.print(F("setenv bootargs \"${bootargs} "
                      "root=mtd:rootfs init=/bin/sh ro "
                      "rootfstype=squashfs console=ttyS0,115200\""));
      break;
    case 6:
      Serial1.print(F("setenv bootargs \"${bootargs} "
                      "rootfs_offset=0x4f7000 rootfs_size=0x252d000 "
                      "coherent_pool=4M cpuidle_sysfs_switch "
                      "pci=pcie_bus_safe rootwait cma=0M\""));
      break;
    case 7:
      Serial1.print(F("printenv bootargs"));
      break;
    case 8:
      Serial.println(F("[BOOT] Starting stock Linux from RAM..."));
      Serial1.print(F("bootm 0x0101f000 - 0x03a24000"));
      break;
    default:
      return;
  }
  Serial1.write('\r');
  Serial1.flush();
  if (step == 8) {
    rootSequence = false;
    ubootReady = false;
    if (fullRequested) {
      setPhase(FP_WAIT_LINUX);
      Serial.println(F("[FULL] Waiting for initial Linux / # prompt..."));
    } else {
      Serial.println(F("[BOOT] Recovery root shell will appear at / #."));
    }
  }
}

void onUBootPrompt() {
  ubootReady = true;
  if (!rootSequence) return;
  if (stepError ||
      (expectedResponse(rootStep) && !stepVerified)) {
    rootSequence = false;
    fullRequested = false;
    Serial.print(F("\n[BOOT] STOPPED: failed U-Boot step "));
    Serial.println(rootStep + 1);
    Serial.println(F("[BOOT] Examine router output before retrying."));
    return;
  }
  if (rootStep + 1 < ROOT_STEPS) sendRootStep(rootStep + 1);
}

void checkUBootPrompt(char c) {
  if (c == '\r' || c == '\n') {
    atLineStart = true;
    promptMatch = 0;
    return;
  }
  if (!atLineStart) return;
  const char prompt[] = "=> ";
  if (c == prompt[promptMatch]) {
    ++promptMatch;
    if (promptMatch == 3) {
      atLineStart = false;
      promptMatch = 0;
      onUBootPrompt();
    }
  } else {
    atLineStart = false;
    promptMatch = 0;
  }
}

void startRootSequence(bool full) {
  if (rootSequence || (fullPhase != FP_IDLE &&
      fullPhase != FP_COMPLETE && fullPhase != FP_FAILED)) {
    Serial.println(F("[BOOT] Automation is already running."));
    return;
  }
  if (!ubootReady) {
    Serial.println(F("[BOOT] No U-Boot => prompt detected."));
    Serial.println(F("[BOOT] Power cycle; wait for =>, or press Enter."));
    return;
  }
  fullRequested = full;
  rootSequence = true;
  warnedInput = false;
  skipNextLF = false;
  setPhase(FP_IDLE);
  Serial.println(full
    ? F("\n[FULL] Starting stock firmware with RAM-only UART root hook")
    : F("\n[BOOT] Starting temporary recovery root shell"));
  Serial.println(F("[BOOT] No saveenv, flash or UBI write commands."));
  if (full) {
    Serial.println(F("[FULL] Normal firmware may write configuration to NAND."));
  }
  sendRootStep(0);
}

// Successful shell command gets a marker that is NOT literally present in
// the echoed command: printf 'HHOK-%d\n' 3 produces HHOK-3.
// This distinguishes completed command output from its UART echo.
const char *hookCommand(uint8_t step) {
  switch (step) {
    case 0: return "printf 'hh_uart(){\\n' >/dev/hh-hook";
    case 1: return "printf 'cp /etc/inittab /tmp/hh-it || exec /bin/sh\\n' >>/dev/hh-hook";
    case 2: return "printf 'echo ttyS0::askfirst:/bin/sh >>/tmp/hh-it || exec /bin/sh\\n' >>/dev/hh-hook";
    case 3: return "printf 'mount -o bind /tmp/hh-it /etc/inittab || exec /bin/sh\\n' >>/dev/hh-hook";
    case 4: return "printf 'grep -q ttyS0::askfirst:/bin/sh /etc/inittab || exec /bin/sh\\n' >>/dev/hh-hook";
    case 5: return "printf '}\\n' >>/dev/hh-hook";
    case 6: return "sed 's/preinit_ip_deconfig/hh_uart; preinit_ip_deconfig/' /lib/preinit/99_10_run_init >>/dev/hh-hook";
    case 7: return "mount -o bind /dev/hh-hook /lib/preinit/99_10_run_init";
    case 8: return "grep -q 'hh_uart; preinit_ip_deconfig' /lib/preinit/99_10_run_init";
    default: return nullptr;
  }
}

bool queueRaw(const char *command) {
  size_t length = strlen(command);
  if (txActive || length + 2 > sizeof(txLine)) return false;
  memcpy(txLine, command, length);
  txLine[length] = '\r';
  txLine[length + 1] = '\0';
  txLength = (uint16_t)(length + 1);
  txPosition = 0;
  txActive = true;
  lastTxMicros = micros() - 500;
  return true;
}

void pumpTx() {
  if (!txActive ||
      (uint32_t)(micros() - lastTxMicros) < 400UL ||
      Serial1.availableForWrite() < 1) return;
  Serial1.write((uint8_t)txLine[txPosition++]);
  lastTxMicros = micros();
  if (txPosition >= txLength) txActive = false;
}

void sendHookStep(uint8_t step) {
  hookStep = step;
  const char *base = hookCommand(step);
  char line[220];
  int n = snprintf(line, sizeof(line),
      "%s && printf 'HHOK-%%d\\n' %u || printf 'HHNO-%%d\\n' %u",
      base, step, step);
  if (!base || n <= 0 || n >= (int)sizeof(line)) {
    failFull(F("Hook command too long / invalid."));
    return;
  }
  snprintf(okToken, sizeof(okToken), "HHOK-%u", step);
  snprintf(noToken, sizeof(noToken), "HHNO-%u", step);
  setPhase(FP_WAIT_HOOK);
  Serial.print(F("[FULL] Installing RAM hook "));
  Serial.print(step + 1);
  Serial.print('/');
  Serial.println(HOOK_STEPS);
  if (!queueRaw(line)) failFull(F("Could not queue hook command."));
}

void handOffToPreinit() {
  Serial.println(F("[FULL] Hook verified; launching normal preinit as PID 1."));
  setPhase(FP_WAIT_INIT);
  if (!queueRaw("exec /etc/preinit")) {
    failFull(F("Could not queue preinit handoff."));
  }
}

void verifyFullBoot() {
  setPhase(FP_VERIFY);
  Serial.println(F("[FULL] UART shell available; verifying PID 1..."));
  if (!queueRaw("test \"$(cat /proc/1/comm)\" = init && printf 'HHREADY-%d\\n' 1 || printf 'HHFAIL-%d\\n' 1")) {
    failFull(F("Could not queue init verification."));
  }
}

void checkFullOutput(char c) {
  switch (fullPhase) {
    case FP_WAIT_LINUX:
      if (matchText(c, "/ # ", shellMatch)) {
        Serial.println(F("\n[FULL] Recovery root shell detected."));
        sendHookStep(0);
      }
      break;

    case FP_WAIT_HOOK:
      if (matchText(c, noToken, noMatch)) {
        failFull(F("A RAM hook command failed."));
      } else if (matchText(c, okToken, okMatch)) {
        if (hookStep + 1 < HOOK_STEPS) sendHookStep(hookStep + 1);
        else handOffToPreinit();
      }
      break;

    case FP_WAIT_INIT:
      if (matchText(c, "Please press Enter to activate this console.", loginMatch)) {
        Serial.println(F("\n[FULL] BusyBox init requested Enter; sending it."));
        Serial1.write('\r');
        setPhase(FP_WAIT_SHELL);
      }
      break;

    case FP_WAIT_SHELL:
      if (matchText(c, "/ # ", shellMatch)) verifyFullBoot();
      break;

    case FP_VERIFY:
      if (matchText(c, "HHREADY-1", readyMatch)) {
        fullRequested = false;
        setPhase(FP_COMPLETE);
        Serial.println(F("\n==============================================="));
        Serial.println(F("[FULL] SUCCESS: stock BusyBox init is PID 1."));
        Serial.println(F("[FULL] UART root shell is interactive."));
        Serial.println(F("[FULL] No firmware image was modified."));
        Serial.println(F("===============================================\n"));
      } else if (matchText(c, "HHFAIL-1", failedMatch)) {
        failFull(F("PID 1 is not BusyBox init."));
      }
      break;

    default:
      break;
  }
}

void checkFullTimeout() {
  unsigned long elapsed = millis() - phaseStarted;
  if (fullPhase == FP_WAIT_LINUX && elapsed > 90000UL) {
    failFull(F("No first Linux root prompt within 90 seconds."));
  } else if (fullPhase == FP_WAIT_HOOK && elapsed > 20000UL) {
    failFull(F("No hook command completion marker within 20 seconds."));
  } else if (fullPhase == FP_WAIT_INIT && elapsed > 120000UL) {
    failFull(F("No BusyBox init login prompt; try Enter manually."));
  } else if (fullPhase == FP_WAIT_SHELL && elapsed > 60000UL) {
    failFull(F("No UART shell prompt after pressing Enter."));
  } else if (fullPhase == FP_VERIFY && elapsed > 15000UL) {
    failFull(F("Could not verify PID 1 automatically."));
  }
}

bool automationBusy() {
  return rootSequence || (fullPhase != FP_IDLE &&
      fullPhase != FP_COMPLETE && fullPhase != FP_FAILED);
}

void sendToRouter(char c) {
  if (c != '\r' && c != '\n') ubootReady = false;
  if (c == '\r') {
    Serial1.write('\r');
    skipNextLF = true;
  } else if (c == '\n') {
    if (!skipNextLF) Serial1.write('\r');
    skipNextLF = false;
  } else {
    skipNextLF = false;
    Serial1.write(c);
  }
}

void setup() {
  Serial.begin(PC_BAUD);
  Serial1.begin(ROUTER_BAUD);
  delay(300);
  showHelp();
  Serial.println(F("[HH4000] Listening at 115200 baud; auto-interrupt armed."));
  Serial.println(F("[HH4000] Power-cycle, then use ~f at the => prompt."));
}

void loop() {
  // Router -> computer, then state-machine analysis.
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    Serial.write((uint8_t)c);
    checkBootMessage(c);
    checkRootResponse(c);
    checkUBootPrompt(c);
    checkFullOutput(c);
  }

  // Computer -> router, with local escape commands.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (swallowLocalEOL && (c == '\r' || c == '\n')) continue;
    swallowLocalEOL = false;

    if (escapeMode) {
      escapeMode = false;
      if (c != '~') swallowLocalEOL = true;
      switch (c) {
        case 's': sendInterrupt(); break;
        case 'a':
          armed = true;
          bootMatch = 0;
          Serial.println(F("\n[AUTO] Armed"));
          break;
        case 'd':
          armed = false;
          Serial.println(F("\n[AUTO] Disarmed"));
          break;
        case 'b': startRootSequence(false); break;
        case 'f': startRootSequence(true); break;
        case 'x': stopAutomation(); break;
        case '?': showHelp(); break;
        case '~':
          if (!automationBusy()) sendToRouter('~');
          break;
        default:
          Serial.println(F("\n[ERROR] Unknown command; use ~?"));
          break;
      }
    } else if (c == '~') {
      escapeMode = true;
    } else if (automationBusy()) {
      if (!warnedInput && c != '\r' && c != '\n') {
        Serial.println(F("\n[AUTO] Input ignored during automation; ~x cancels."));
        warnedInput = true;
      }
    } else {
      sendToRouter(c);
    }
  }

  pumpTx();
  checkFullTimeout();
}
