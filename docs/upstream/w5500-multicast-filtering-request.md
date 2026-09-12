# Draft: Zephyr feature request — multicast blocking for W5500

Status: draft, not submitted. Target: github.com/zephyrproject-rtos/zephyr, label `area: Ethernet`.
Evidence behind the proposal, if anyone asks on the thread: `w5500-filtering-research.md`.

---

**Title:** `drivers: ethernet: w5500: allow blocking multicast in MACRAW mode`

Hi all,

The W5500 driver currently does no hardware multicast filtering. The chip has the `MMB` and
`MIP6B` bits in `Sn_MR` for blocking IPv4 and IPv6 multicast in MACRAW mode, but the driver never
touches them, so every multicast frame on the segment is pulled over SPI and dropped later in
software.

### Why this is worth having

On an unmanaged switch every multicast frame is flooded to all ports much like a broadcast, while
keeping its multicast headers. With filtering left to software, the driver has to receive and
process all of it over a slow SPI link. Even a modest stream is enough to paralyse the device and
its networking.

Most devices will never join an IPv4 multicast group, and many will never use IPv6 at all, so for
them this traffic is pure waste that the hardware could drop for free.

### Why not the standard filtering API

Zephyr already has `ETHERNET_HW_FILTERING` with `ETHERNET_CONFIG_TYPE_FILTER`, driven by the L2
layer on group join and leave. It does not fit here: in MACRAW the W5500 has no per-address
multicast filter and no hash table, only these blanket category bits. Claiming
`ETHERNET_HW_FILTERING` would promise per-group filtering the hardware cannot deliver.

### Proposal

Expose the two bits as optional devicetree properties on the `wiznet,w5500` node, defaulting to
current behaviour so nothing changes for existing users:

```yaml
  disable-v4-multicast:
    type: boolean
    description: |
      Set MMB in Sn_MR so the controller drops IPv4 multicast in MACRAW mode.
      Note this also blocks IPv4 mDNS.

  disable-v6-multicast:
    type: boolean
    description: |
      Set MIP6B in Sn_MR so the controller drops IPv6 multicast in MACRAW mode.
      Note that IPv6 Neighbour Discovery and SLAAC use multicast, so enabling
      this effectively disables IPv6 on the interface.
```

Devicetree rather than Kconfig, because whether this is wanted depends on the network the part is
wired into, not on the build.

Happy to prepare the patch if maintainers agree on the shape.
