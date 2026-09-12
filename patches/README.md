# Release manifest: what this product changes outside its own repository

A build of this firmware is not reproducible from this repository alone. Three
edits live in the `zephyr` checkout, which this product does not own and which
is shared with several unrelated projects in the same workspace. Anything that
resets or re-clones that repository removes all three, and the build either
fails or — worse, in the case of the W5500 patch — succeeds and misbehaves on
the bench.

This directory holds every one of those edits so they can be restored, and this
file is the record of what they are, why they exist and when each can be
dropped.

| What | Where it lives | Restored by | Drop it when |
|---|---|---|---|
| W5500 IPv4 multicast blocking | `zephyr/drivers/ethernet/eth_w5500.c`, `eth_w5500_priv.h` | `west patch apply`, see below | Upstream accepts the request in `docs/upstream/`, or [#115626](https://github.com/zephyrproject-rtos/zephyr/issues/115626) makes the RX thread preemptible |
| Manifest entries for Matter and the three Cedar repositories | `zephyr/west.yml` | `git apply` of `workspace/west.yml.patch` | The workspace moves to an application-owned manifest repository |
| esp-serial-flasher submanifest | `zephyr/submanifests/esp-serial-flasher.yaml` | copy from `workspace/submanifests/` | Same as above |

## Layout

```
patches/
  patches.yml                                          west patch definitions
  zephyr/
    eth_w5500-block-ipv4-multicast-in-macraw-mode.patch
  workspace/
    west.yml.patch                                     manifest edit
    submanifests/esp-serial-flasher.yaml               verbatim copy
```

Only the first is a `west patch` entry. The other two are not patches against a
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
# 2 files changed, 11 insertions(+), 2 deletions(-)
```

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
