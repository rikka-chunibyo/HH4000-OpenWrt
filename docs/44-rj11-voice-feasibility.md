# RJ11 / FXS voice feasibility audit

Read-only router audit on 2026-11-11. No voice drivers, programs, SIP
listeners, ringing, or audio sessions were started. No router files or
flash were changed. Dev-host changes: this report, the docs index/cross
reference, and `work/voice-audit/static-inventory.json`.

## What the ports are

Stock logs identify `SI32282_CB`, a Silicon Labs Si32282 dual FXS line
interface. The two RJ11 telephone ports provide the analog-phone side of
an ATA. A working driver/media/signaling stack would let ordinary analog
phones make and receive network calls. SIP registration/dial plans and
Home Assistant integration are software work; the RJ11 sockets alone do
not provide them in the installed OpenWrt image.

Older doc 04 described voice as unsupported in a mainline-kernel port.
That does not rule out reuse on our retained vendor kernel.

## Verified local assets

* `work/rootfs/lib/modules/4.1.52-5.02L.07/slicslac.ko`: stock SLIC driver.
* `dsphal.ko` in the same directory: stock PCM/DSP hardware interface.
* Both modules have the currently running kernel's vermagic and their
  files are already present in installed OpenWrt, but neither is loaded
  and neither `/dev/slicslac` nor `/dev/dsphal` exists at audit time.
* `work/rootfs/usr/lib/libh2alvoice.so.0.0.0` exports endpoint creation,
  capability queries, calibration, ring/tones, DTMF, voice connections,
  RTP sessions and remote-address controls. The `h2alvoice-cli` test tool
  contains ONHOOK/OFFHOOK callbacks and controls for these operations.
* Its 20-library dynamic dependency closure is available in the backup.
  This does not establish independence from all runtime services,
  configuration, initialization, firmware, or calibration requirements.
* `voiceManager`, `libconfigvoice`, `libhsalvoice` and several IPC/config
  libraries form a higher-level stock SIP stack. Reusing the entire
  manager may couple us to Sagemcom middleware; a small hardware adapter
  around H2ALVOICE is a plausible alternative, not an implemented one.
* The HAL contains defined executable functions `opus_encode`,
  `opus_decode`, `opuswrap_Encode`, and `opuswrap_Decode`. However, the CLI
  codec choices list PCMU/PCMA/G729/G722/AMR-WB, not Opus. The presence of
  the Opus implementation does not prove it is selectable for this
  board's analog endpoints or interoperable with Home Assistant.

## SIP and Home Assistant

A configurable ATA could register to an independent SIP provider or local
PBX over Ethernet WAN; this does not intrinsically require Bell's PON
service. Core possibilities include normal calls, off-hook auto-dial to
an assistant, dialed automation extensions, and PBX-originated ringing or
spoken notifications. Grandstream-equivalent feature completeness,
codec/security/fax behavior and either port's actual media path remain
untested on the HH4000.

Home Assistant officially documents analog-phone Assist through its
[Voice over IP integration](https://www.home-assistant.io/integrations/voip/)
and a [Grandstream tutorial](https://www.home-assistant.io/voice_control/worlds-most-private-voice-assistant/).
The tutorial calls for Opus. If the stock endpoint exposes a usable Opus
path, direct interoperability is plausible. Otherwise a local PBX/media
bridge could translate between a working HH4000 codec and Opus; that is a
proposed architecture, not a tested workaround. Home Assistant/the chosen
assistant server would perform speech recognition, automation/AI logic,
and speech synthesis; the router would supply the phone interface.

## Useful next experiments

1. In an isolated RAM test, initialize the two stock drivers and a private
   copy of the 32-bit vendor voice runtime. Inspect capability/board setup
   and callbacks using the saved CLI as a guide.
2. With an analog handset on TEL1, prove off-hook/on-hook, keypad events,
   dial tone, and ringing.
3. Prove bidirectional PCM/RTP audio with a simple supported codec before
   implementing SIP registration or adding another large daemon.
4. Connect a configurable SIP endpoint to a local test PBX; then validate
   direct Home Assistant Opus or a media-conversion route. Test TEL2 and
   additional calling features after the first complete call works.

This is a concrete reuse lead with substantial assets available, but not
an enabled ATA. Installing a generic Asterisk package alone will not make
these proprietary FXS devices usable; a matching hardware integration is
still required. There is not enough runtime evidence for a reliable time
estimate or to promise full Grandstream feature parity.
