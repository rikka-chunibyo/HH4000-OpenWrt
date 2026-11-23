# Buttons and button LEDs: live verification

The user pressed UP, OK, DOWN and the main ring button in sequence. Logs from the running stock OS confirmed UP=button index 8/GPIO13, OK=10/GPIO15 and DOWN=9/GPIO14. The main ring button is separate: the MCU input device `/dev/input/event0`, name `mcu-keys`, emits EV_KEY code **247**. `/dev/but0` did not change during that main-button press; it is the separate reset path.

The MCU reported repeated value **2** while held, then **1** and **0** at release. A custom menu daemon must coalesce repeated value 2, not treat each as a new press. Static code also defines value 3 for a long hold; no long hold was tested because the stock service assigns a reboot action. The menu GPIOs were read noninvasively through the state ioctl; we did not open their callback/event devices and displace the vendor callback.

The physical camera scan confirmed the **main ring can be red, blue and white and can be completely off**. UP and DOWN have white channels. OK has white, red and a firmware-labelled yellow channel that appears orange/amber. Each channel was selected individually; the saved GPIO states were restored and Bell's UI resumed afterward. Multiple simultaneous colour combinations and dimming have not been characterized.

The exact GPIO/name-id map and ioctl formats are in hardware-results.json. The reusable helper is [hh4000_io.c](../tools/hh4000_io.c), with a working ARM32 build at `work/io-live/hh4000-io.arm`. Its `info`, `watch` and bounded `scan` modes query state, monitor button changes and cycle/restore the known LED channels. The device is `/dev/led`.

**ABI detail:** the native AArch64 helper could not use these ioctls; the ARM32 build succeeds. A future AArch64/musl status daemon can communicate with a small ARM32/glibc helper, or we can add a native-compatible interface to a rebuilt GPIO module. The message structures are fixed-width uint32 values, but the driver's ioctl registration path still matters. Preserve the existing chip-specific driver behavior until a replacement has been validated.

BSP name IDs used by the GPIO ioctls are not GPIO numbers or HAL LED indices. For example main red uses BSP name ID 54, GPIO77, HAL LED27. Mixing these namespaces would address the wrong function.

Evidence: button log, LED sequence, red ring photograph, blue ring, white OK, red OK, amber OK, white DOWN. The ring is dark in the OK/DOWN photographs, proving software can turn it off while the router remains powered.

For the planned OpenWrt status system, keep three interfaces: a renderer (display text/images/menu), normalized button events (up/down/enter/main short/main long), and named LED states. Define hold actions in our daemon. Keep hardware-state reads and network status providers separate from menu actions so a UI crash cannot change network configuration. The current stock driver helper is a working backend reference; the complete status/menu application is not implemented yet.
