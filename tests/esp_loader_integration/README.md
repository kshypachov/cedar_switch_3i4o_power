# esp-serial-flasher integration check (P0)

Verifies that `espressif/esp-serial-flasher` is wired correctly to the on-board
ESP32-C6 before any updater code exists. Running it early separates integration
problems from bugs in our own code later.

Non-destructive: it enters the ROM download mode, identifies the chip and
returns it to normal boot. Nothing is erased or written, so it is safe to run
on a board carrying production coprocessor firmware.

## What a pass proves

- The west module resolves and builds for this board.
- The `espressif,esp-loader` node points at the right UART and straps.
- EN/BOOT timing actually gets the C6 into its ROM loader.
- The serial protocol completes a round trip and reports the expected chip.

## Build and run

```sh
west build -p always \
  -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
  cedar_switch_3in4out_power/tests/esp_loader_integration
```

Console output lands on USART1 at 115200, the board's usual console.

```sh
west twister -T cedar_switch_3in4out_power/tests/esp_loader_integration \
  -p cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app --device-testing
```

## Notes

The overlay disables `/uart-bridge0`. The board pairs USART3 with the USB
CDC-ACM bridge, and two owners of one UART is exactly what must not happen
while flashing. In the application the bridge is suspended for the duration of
an update instead of being disabled at build time.

`reset-gpios` and `boot-gpios` are also listed under the board's `gpio-outputs`
node. That node's driver is not enabled in this build, so the loader owns the
pins exclusively here. In the application the hand-off is explicit and belongs
to `coprocessor-manager`.
