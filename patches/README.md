# Release manifest: what this product changes outside its own repository

A build of this firmware is not reproducible from this repository alone. The
seven edits in the table below live in the `zephyr` checkout, which this product
does not own and which is shared with several unrelated projects in the same
workspace. Anything that resets or re-clones that repository removes all of
them, and the build either fails or — worse, in the case of the W5500 patches —
succeeds and misbehaves on the bench.

This directory holds every one of those edits so they can be restored, and this
file is the record of what they are, why they exist and when each can be
dropped.

| What | Where it lives | Restored by | Drop it when |
|---|---|---|---|
| W5500 IPv4 multicast blocking | `zephyr/drivers/ethernet/eth_w5500.c`, `eth_w5500_priv.h` | `west patch apply`, see below | Upstream accepts the request in `docs/upstream/`, or [#115626](https://github.com/zephyrproject-rtos/zephyr/issues/115626) makes the RX thread preemptible |
| W5500 socket reopened when the receive state is inconsistent | `zephyr/drivers/ethernet/eth_w5500.c` (`w5500_rx`) | `west patch apply`, see below | [#115626](https://github.com/zephyrproject-rtos/zephyr/pull/115626) is merged — its rewrite of `w5500_rx` compares the length with `Sn_RX_RSR` |
| ESP-Hosted link restart after the ESP32-C6 is re-flashed (`esp_hosted_mcu_restart`, `esp_hosted_mcu_wifi_restart`) | `zephyr/drivers/misc/esp_hosted_mcu/esp_hosted_mcu.{c,h}`, `esp_hosted_mcu_spi.c`, `zephyr/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c` | `west patch apply` of that one entry, see below; made against the driver as this checkout already carries it | The upstream driver can re-initialise its link and Wi-Fi after a coprocessor restart |
| ESP-Hosted receive thread: suspended while the ESP32-C6 is in its ROM loader (`esp_hosted_mcu_suspend`), and a sleep after a burst that carried no frame | `zephyr/drivers/misc/esp_hosted_mcu/esp_hosted_mcu.{c,h}` | `west patch apply` of that one entry, after the restart entry | The upstream receive thread stops treating a high data-ready line without frames as work, or offers a suspend |
| OCTOSPI NOR program/erase while executing from it (`FLASH_STM32_OSPI_XIP`) and `sys_clock_systick_wraps_observed()` — the STM32 update writes `image_ok` into slot 1 through it | `zephyr/drivers/flash/flash_stm32_ospi.c`, `flash_stm32_ospi_xip.{c,h}`, `Kconfig.stm32_ospi`, `CMakeLists.txt`; `zephyr/drivers/timer/cortex_m_systick.c`, `include/zephyr/drivers/timer/system_timer.h` | `west patch apply` of that one entry, see below; recorded from the tree, where it has been since 2026-09-06 | The upstream STM32 OSPI driver can write while memory-mapped and executing in place |
| Manifest entries for Matter and the three Cedar repositories | `zephyr/west.yml` | `git apply` of `workspace/west.yml.patch` | The workspace moves to an application-owned manifest repository |
| esp-serial-flasher submanifest | `zephyr/submanifests/esp-serial-flasher.yaml` | copy from `workspace/submanifests/` | Same as above |

## Layout

```
patches/
  patches.yml                                          west patch definitions
  zephyr/
    eth_w5500-block-ipv4-multicast-in-macraw-mode.patch
    eth_w5500-reopen-socket-on-inconsistent-rx.patch
    flash_stm32_ospi-xip-safe-program-erase.patch
  workspace/
    west.yml.patch                                     manifest edit
    submanifests/esp-serial-flasher.yaml               verbatim copy
```

Only the two under `zephyr/` are `west patch` entries. The other two are not patches against a
module — one edits the manifest that west itself reads, the other is a new file
west has no record of — so they are restored by hand and kept here verbatim.

## Restoring after a reset of the zephyr checkout

From the workspace root (`/Volumes/Programming/Zephyr/zephyr_latest` on the
development host):

```sh
APP=cedar_switch_3in4out_power

# 1. Manifest entries and the submanifest, before anything reads the manifest.
git -C zephyr apply "$APP/patches/workspace/west.yml.patch"
cp "$APP/patches/workspace/submanifests/esp-serial-flasher.yaml" \
   zephyr/submanifests/

# 2. Projects, now that the manifest names them.
west update

# 3. The driver patch.
west patch -sm "$APP" -b patches -l patches/patches.yml -dm zephyr apply
```

`-sm` takes a directory here, not the west project name: the application has no
`zephyr/module.yml`, so west does not recognise `cedar_switch_3i4o_power` as a
module and the workspace-relative path is used instead. `-dm zephyr` keeps the
command from walking every other module in the workspace.

Verify:

```sh
git -C zephyr diff --stat -- drivers/ethernet/eth_w5500.c drivers/ethernet/eth_w5500_priv.h
# 2 files changed, 38 insertions(+), 4 deletions(-)
```

### One new entry on a checkout that already has the others

`west patch apply` runs every entry and stops at the first that fails, and an
entry already applied fails. Its `-r` would then roll back — through the clean
command, `git clean`. So a new entry goes on by itself: a list holding only that
entry, inside the workspace (`-l` is refused as an absolute path together with
`-sm`, and `-sm` must be inside the workspace), and never `-r`. P4 applied the
frame-length patch this way:

```sh
cp one-entry.yml "$APP/patches/apply-one.yml"
west patch -sm "$APP" -b patches -l patches/apply-one.yml -dm zephyr apply
rm "$APP/patches/apply-one.yml"
git -C zephyr status --short   # the same lines as before; only the diff of the patched file grew
```

`west patch list` validates the whole file against the schema first — run it
after editing `patches.yml`: a key the schema does not define (`pr` instead of
`merge-pr`) makes every later `apply` fail.

**Never run `west patch clean` in this workspace.** Its default clean command is
`git clean -d -f -x`, and the zephyr checkout holds untracked work from other
projects — including `submanifests/esp-serial-flasher.yaml`, without which this
product does not configure.

## The W5500 patch

Socket 0 already runs in MACRAW with the MAC filter on. The patch additionally
sets MMB, `Sn_MR` bit 5, so the chip drops IPv4 multicast before it reaches the
host.

It exists because of a measured failure, not a theoretical one. The driver's RX
thread is created `K_PRIO_COOP(2)` and performs synchronous polled SPI inside
`while (int_pin)` without yielding, so frame rate translates directly into
starvation rather than load. An unrelated 239.255.x.x stream on the LAN was
delivering roughly 271 frames/s; logging and the shell stopped responding for as
long as it ran. Every one of those frames was discarded by the IP stack — none
were addressed to this device — so the whole cost was paid before anything could
decide they were uninteresting.

MIP6B, bit 4, is deliberately left clear. Both bits were characterised on this
board rather than taken from the datasheet, by two independent methods: MMB
drops only IPv4 multicast, MIP6B only IPv6 multicast. Blocking IPv6 multicast
breaks Neighbor Discovery and SLAAC, and Matter runs on IPv6 multicast, so
MIP6B cannot be used on this product at all. `docs/upstream/` holds the
measurements and the drafted upstream request.

This patch removes the traffic that triggered the starvation. It does not fix
the starvation: any other sustained flow on the segment reaches the same state.
That is upstream issue #115626, where the position is already that these threads
should not be cooperative, and waiting for that is cheaper than carrying a
scheduling fix of our own in a foreign driver.

## The receive-state patch

`w5500_rx()` reads the two-byte frame length from the chip's MACRAW header and
copies that many bytes into a packet it allocated for the length the header
claimed — but never compares the length with `Sn_RX_RSR`, the bytes actually
buffered, or with `NET_ETH_MAX_FRAME_SIZE`. When they disagree, the copy walks
past the last fragment and dereferences a NULL buffer: `BUS FAULT`, `BFAR 0xc`,
`eth_w5500.c:256`. P0 saw it after a coprocessor reset; board B in P4 hit it on
every boot, 9 s in (`docs/device-development/reports/p4/hw`). A length of 2 or
less is only logged, and the read pointer stays where it was.

On board B those moments read `Sn_RX_RSR` as 65535 and the header as 65533 —
all-ones, on the SPI bus the W5500 shares with a C6 that has no firmware. The
first version of the patch discarded the buffered bytes by advancing `Sn_RX_RD`
by `Sn_RX_RSR`; with that value the pointer left the data, every later header
read as 0, and the interface received nothing, IPv4 or IPv6, until reset.

The patch now closes socket 0 and opens it again whenever the header is 2 or
less, larger than `Sn_RX_RSR`, or longer than a frame. OPEN resets the chip's
receive pointers; `Sn_MR` survives CLOSE, so MACRAW mode and the filters stay.
Each of the two commands can busy-wait up to 100 ms in the cooperative RX thread
while the bus reads wrong. Why the reads go wrong is not established.

It is not sent upstream on its own because PR #115626 rewrites `w5500_rx()` with
a comparison against `Sn_RX_RSR`; the entry goes when that PR is merged.

## Adding an entry

Generate the patch against the pristine file, not against another patch:

```sh
git -C zephyr diff -- <paths> >> patches/zephyr/<name>.patch
shasum -a 256 patches/zephyr/<name>.patch
```

Prepend the `From:`/`Subject:` header block so the file explains itself, record
the checksum in `patches.yml`, and prove it round-trips before committing —
revert the files, run `west patch apply`, and confirm the diff is unchanged.
Then add a row to the table above with the condition under which the patch goes
away, because an entry with no exit condition is how a fork starts.
