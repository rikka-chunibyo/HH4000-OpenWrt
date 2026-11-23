# Reset button investigation

> Historical investigation. The configurable reboot and guarded full factory reset are now implemented; see [39-configurable-reset-and-factory-reset.md](39-configurable-reset-and-factory-reset.md). Statements below about unsupported reset describe the earlier state.

Existing BSP research maps rear RESET to SoC GPIO59, active-low, button index0. This is distinct from the MCU main button. The current front-panel service reads only BSP indices8/9/10 and MCU key247, so it does not currently consume reset events.

The stock generic `/etc/rc.button/reset` exists and contains short-release reboot and long-hold factory-reset actions. No GPIO reset input device was present in `/proc/bus/input/devices`; only mcu-keys was present. To make the physical test logging-only, a no-action handler is bind-mounted over the generic reset script in RAM. This temporary mask disappears on reboot or explicit unmount; the original file was not rewritten.

A read-only native ARM32 listener uses the previously verified GPIO-read ioctl `0xc004cc03` on `/dev/led`, index0. It makes no GPIO writes, debounces edges for60ms and logs press/release with monotonic uptime and held duration. It exits after1800seconds. Initial reading was released. The physical press is still pending; the mapping is not yet claimed as live-confirmed.

Runtime files: `/tmp/hh-reset-listener/{pid,events.jsonl,hotplug.log,reset-handler}`. Listener PID at startup7861. Source: `tools/reset_button_logger.c`. Build: `work/reset-button/reset-button-logger`. Inspect events before changing the panel API or assigning actions. Do not connect this key to generic factory reset: factory reset remains unsupported for the custom data layout.

Router flash was not written. Only a temporary listener and RAM bind mount were added. Dev source/build/documentation artifacts were created. On completion, stop the recorded listener if still running, inspect the log, then explicitly unmount `/etc/rc.button/reset` when restoring the original handler is appropriate.

## Physical confirmation

The user pressed RESET once. The read-only index0 listener reported press at uptime667.580 and release at668.040, duration0.460seconds. This live-confirms the BSP index0 input for the physical reset button, separate from the MCU main key. No generic button-hotplug callback was observed. No reboot or reset action was assigned. The temporary listener is stopped and its RAM handler mask removed after capture; the pre-existing generic handler remains dormant on the current input path. Evidence: `work/reset-button/physical-test.json`.
