# Backing material: W5500 MACRAW filtering

Internal notes, not for submission. The short request we actually post is in
`w5500-multicast-filtering-request.md`; this file holds the evidence behind it, for answering
questions if any come up on the thread.

Related upstream work: issue #115626 (RX draining and cooperative threads in the same driver).

---

**Title:** `drivers: ethernet: w5500: expose the MACRAW multicast/IPv6 blocking bits`

Hi all,

The W5500 driver currently does no hardware multicast filtering. The chip has the
`MMB` and `MIP6B` bits in `Sn_MR` for exactly this, but the driver never touches them,
so every multicast frame on the segment is pulled over SPI and dropped later in software.

### Why this matters

On an unmanaged switch every multicast frame is flooded to all ports much like a
broadcast, while keeping its multicast headers. With filtering left to software, the
driver has to receive and process all of it. Even a modest stream is enough to paralyse
the device and its networking.

We hit this on a custom STM32U585 board with a W5500 on SPI. An unrelated IPv4 multicast
stream on the segment (groups 239.255.12.2/4, ~2.7 Mbit/s) produced **~271 frames/s of
~1256 bytes**. The chip's RX buffer stayed permanently 9-10 KB backed up and the whole
system was starved: the logging thread and the shell never ran, ICMP echo took
245-450 ms with up to 33% loss, and a trivial HTTP response took 0.65-0.83 s.

Counters compiled into the driver showed the RX path itself was healthy — zero allocation
failures, zero bad headers, zero read errors. Every interrupt was a real frame that the
IP stack then discarded, because nothing on the host had joined those groups.

Setting `MMB` removed the load completely: ICMP echo **8.5-11.6 ms with 0% loss**,
HTTP **p50 53 ms / p95 56 ms**.

Worth noting for anyone reading the datasheet: `MFEN` is already enabled by the driver,
and it does **not** filter multicast, despite the wording "receives only broadcast packets
or packets addressed to it". That was verified on hardware.

### Why the standard filtering API does not fit

Zephyr already has `ETHERNET_HW_FILTERING` with `ETHERNET_CONFIG_TYPE_FILTER`, driven
automatically by the L2 layer on group join/leave and implemented by `eth_nxp_enet`,
`eth_stm32_hal_v2`, `eth_adin2111` and others.

It cannot be implemented honestly on this part: in MACRAW the W5500 has no per-address
multicast filter and no hash table. It can only block whole categories of frames.
Claiming `ETHERNET_HW_FILTERING` would promise per-group filtering the hardware cannot
deliver.

### What the two bits actually do

This is where it gets awkward, because WIZnet's own documents disagree with each other.

The W5500 datasheet v1.1.0 (p. 44-45) says:

| Bit | Name | W5500 datasheet wording |
|----:|------|-------------------------|
| 5 | `MMB` | "blocks to receive the packet with multicast MAC address" |
| 4 | `MIP6B` | "blocks to receiving the IPv6 packet" |

The same two bit positions on the successor W6100 (datasheet v1.0.5, pp. 55-56) are documented
quite differently:

| Bit | Name | W6100 datasheet wording |
|----:|------|-------------------------|
| 5 | `MMB` | "UDP4 Multicast Block in MACRAW Mode ... 1 : Block **IPv4** Multicast" |
| 4 | `MMB6` | "UDP6 Multicast Block in MACRAW Mode ... 1 : Block **IPv6** Multicast" |

And on p. 115 of that same W6100 datasheet, WIZnet's own MACRAW pseudo-code comments the bit
their register table defines as "Block IPv6 Multicast" like this:

```
S0_MR[MMB]  = '1';   // Multicast Packet Block
S0_MR[MMB6] = '1';   // IPv6 Packet Block
```

`// IPv6 Packet Block` is the W5500's exact `MIP6B` phrasing. In the same code block, `UNIB`
(Unicast Block) is commented "Broadcast Packet Block". So the W5500 register text looks less
like a specification of different behaviour and more like the loose ancestor of wording WIZnet
later made precise — and they still contradict themselves about it inside a single page.

There is independent hardware evidence for bit 5. Espressif's W5500 driver carries this comment,
from `esp-eth-drivers/w5500/src/esp_eth_mac_w5500.c`:

> W5500 filters out all multicast packets except for IP multicast. However, behavior is not
> consistent. IPv4 multicast can be blocked, but IPv6 is always accepted (this is not documented
> behavior, but it's observed on the real hardware).

together with `ESP_LOGW(TAG, "IPv6 multicast is always filtered in by W5500.")`, and, in
esp-eth-drivers PR #133, "the W6100 supports blocking v6 multicast, the W5500 can only block v4".
RIOT-OS corroborates by shipping bit 5 set unconditionally while running GNRC, which is IPv6-only
and needs solicited-node multicast for Neighbour Discovery.

Our own measurements on an STM32U585 + W5500 agree with all of the above for bit 5.

For bit 4 we could find no published measurement by anyone, on any platform. Our results for it
are reported below and should be treated accordingly.

### What we measured

We characterised both bits on an STM32U585 + W5500 by two independent methods, and they agree.

**Method 1 — frame census inside the driver.** Counters in `w5500_rx()` classified every frame the
chip delivered, by destination MAC and, for unicast, by ethertype. `Sn_MR` and `Sn_SR` were read
back from the chip after `OPEN` in every build to confirm the configuration actually took
(`0x84` / `0xa4` / `0x94` / `0xb4`, `Sn_SR = 0x42`). Counters were read over SWD, because the
board's logging thread is starved whenever the multicast flood reaches the driver.

Idle windows, 10 s each, ambient traffic only:

| `Sn_MR` | IPv4 multicast delivered | IPv6 multicast delivered |
|---|---|---|
| `MF` | 2929 (292 f/s) | 4 |
| `MF\|MMB` | **0** | 16 |
| `MF\|MIP6B` | 2906 / 2942 | **0** |
| `MF\|MMB\|MIP6B` | **0** | **0** |

With `MIP6B` set, the unicast-IPv6 counter kept incrementing throughout (12, 11, 12, 14 frames
across runs), including for UDP as well as ICMPv6. Frames to the solicited-node group
`ff02::1:ff10:1273` produced **zero** deliveries.

**Method 2 — outside the device, plus the stack's own instrumentation.** From a wired peer whose
neighbour cache had never held an entry for the board:

| Probe | `MF\|MMB` | `MF\|MMB\|MIP6B` |
|---|---|---|
| `ping6` link-local, cold peer | 5/5, NDP entry created | **0/5, entry never created** |
| `ping6` board ULA | 5/5 | 0/5 |
| Board's ULA address | `fd27:…:8234:28ff:fe10:1273` present | **none; no prefixes at all** |
| `ping6 ff02::1` | board replies | board silent |
| Stack log for that probe | `Received Echo Request … to ff02::1` | **no ff02 receipt logged at all** |
| `net stats` IPv6 recv, 5 probes | +6 | **+0** |
| IPv4 ping / HTTP | fine | fine |

The stack-level evidence rules out an alternative explanation we took seriously: that the replies
were lost inside Zephyr's ICMPv6 source-address selection rather than in the chip. In the blocked
configuration the stack never saw the packet at all (`DROP: No src address match` appears zero
times), and in the control configuration that same branch ran and succeeded.

**Conclusion: on this part `MMB` blocks IPv4 multicast only, and `MIP6B` blocks IPv6 multicast
only.** Unicast IPv6 passes with `MIP6B` set. That matches the W6100 definitions exactly and
contradicts the W5500 datasheet's unqualified wording for bit 4.

**But "multicast only" badly understates what `MIP6B` costs.** Neighbour Discovery and SLAAC both
ride on multicast, so with the bit set the board never receives a Neighbour Solicitation, never
gets a Router Advertisement prefix, and is unreachable over IPv6 to any peer without a
pre-existing cache entry. It merely leaves already-established unicast conversations alive, which
is what makes it look harmless when observed from inside the driver. Treat `MIP6B` as "IPv6 off".

### A related documentation defect: MFEN and multicast

While reading these, note that the W5500 datasheet describes `MFEN` (bit 7) as "W5500 can only
receive broadcasting packet or packet sent to itself" — omitting multicast. Its siblings do not:

- W5100S v1.2.5 p. 40: "block all Packets **without Multicast**, Broadcast and the Packets no having Source MAC"
- W6100 v1.0.5: "Receive only **Multicast**, Broadcast and Source MAC(SHAR) Address Packets"

Mainline Linux (`drivers/net/ethernet/wiznet/w5100.c`) and Zephyr both run MACRAW with `MFEN` set
and working IPv6 Neighbour Discovery, which is only possible if `MFEN` passes multicast. The
W5500's phrasing appears to be an omission rather than a behavioural difference — and it is the
reason a reader may wrongly conclude that the driver already filters multicast today.

### Proposal

Since many devices will never need IPv4 multicast, and some will never need IPv6 at all,
expose both bits as optional devicetree properties on the `wiznet,w5500` node, defaulting
to current behaviour so nothing changes for existing users:

```yaml
  disable-v4-multicast:
    type: boolean
    description: |
      Set MMB in Sn_MR so the controller drops IPv4 multicast in MACRAW mode.
      Useful on segments where an unmanaged switch floods multicast the host has
      not joined. Note this also blocks IPv4 mDNS.

  disable-v6-multicast:
    type: boolean
    description: |
      Set MIP6B in Sn_MR so the controller drops IPv6 multicast in MACRAW mode.
      Measured effect: the device stops receiving Neighbour Solicitations and
      Router Advertisements, loses its SLAAC address and becomes unreachable to
      any IPv6 peer without a pre-existing neighbour cache entry. Only already
      established unicast conversations survive. Treat this as disabling IPv6.
```

The names describe the measured behaviour rather than the datasheet wording. If
maintainers prefer names tied to the register bits, `block-mmb` / `block-mip6b` would do
just as well - but they would tell users less.

Devicetree seems the better fit than Kconfig, because whether this is wanted depends on
the network the part is wired into, not on the build. Happy to prepare the patch if
maintainers agree on the shape, and to run whatever hardware experiment would settle the
exact scope of each bit.

### Environment

- Zephyr `v4.4.0-12983-gb6a5e6e8aa90`
- Custom STM32U585 board, W5500 on SPI2, MACRAW, interrupt mode
