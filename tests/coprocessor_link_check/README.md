# ESP32-C6 link check

Board bring-up diagnostic for a freshly soldered board. Run it before anything
that assumes the coprocessor works, and when the loader cannot reach the C6.

It answers three questions without involving esp-serial-flasher, so a failure
here points at the hardware rather than at our software:

1. Do the control pins move? Reset and boot straps are read back after being
   driven.
2. Does the C6 talk? Its output on USART3 is dumped raw for both a normal boot
   and a download-mode boot.
3. If nothing arrives, is the line driven or dead? PC11 is sampled with an
   internal pull-up and then a pull-down.

## Reading the result

| PC11 pull-up / pull-down | Meaning |
|---|---|
| 1 / 1 | Driven high: idle UART, the module is alive |
| 1 / 0 | Floating: nothing drives the line, the module is not connected |
| 0 / 0 | Held low |

A working module produces several kilobytes on a normal boot: the ROM banner,
then the ESP-IDF second-stage bootloader, then whatever firmware it carries.

## Field result, 2026-09-12

Two boards were compared. On the first, PC11 read 1/0 in every state and not a
single byte or edge arrived, with the control pins toggling correctly - the C6
was not driving the line at all, a hardware fault. On the replacement board the
same build produced 6255 bytes on a normal boot and PC11 read 1/1.

## Build and run

```sh
west build -p always \
  -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
  cedar_switch_3in4out_power/tests/coprocessor_link_check
```

Console on USART1 at 115200.
