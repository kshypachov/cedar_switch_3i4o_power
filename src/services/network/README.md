# Network service

The board's side of `modules/network-manager`: which pieces make this device's
network, and the one thread that does all its interface work.

Plan: sections 3 (adapters in `src/services/`), 5 (network and applying
settings) and 13 (the owner's recovery rule). Module: `network_manager.h`.

## Pieces

| File | What | Tier |
|---|---|---|
| `network_service.c` | Start-up order, the worker, the boot count | hardware |
| `net_adapter.c` | `struct network_iface_ops` over the W5500 and the ESP-Hosted Wi-Fi driver | hardware |
| `netcfg_store.c` | device-config-store's three slots in settings-registry | sim (`tests/network_service`) |
| `boot_streak.c` | Five short boots in a row restore the network configuration | sim |
| `net_adapter_map.c` | Wi-Fi security types and netmasks, translated | sim |

## Start-up

`main()` runs, in this order: `setting_registry_init()`, `job_manager_init()`,
`ethernet_interfaces_init()` (the W5500's EEPROM MAC and interface up, no
addressing), `network_service_start()`, then `app_web_init()`.

`network_service_start()` starts the worker and runs the rest as its first job,
on the worker's 8 KiB stack, returning only when that job has finished — so
network-manager is ready before the web API starts. Run on `main()`'s 2 KiB
stack, step 1 overflowed it on board B: two 512-byte slot records and the
store's own records are on the stack at once.

1. Loads device-config-store from settings-registry. A change that was applied
   and not confirmed before the reboot is rolled back here, and its id is kept
   for a client still polling it (410 `boot_changed`).
2. Binds the adapter and network-manager.
3. Counts the boot towards the physical recovery. The count is stored before
   anything else; the fifth short boot in a row restores the factory network
   configuration (Ethernet DHCP, automatic DNS, Wi-Fi off with its password
   cleared). A boot that stays up 30 s clears the count.
4. Starts the worker, whose first pass puts the committed configuration on the
   interfaces. A device that never had one comes up on Ethernet DHCP.

Addresses changing never restart Matter: the IPv6 link-local address of the
first interface-up starts it once (`start_matter()`, guarded), and nothing in
this service touches the Matter stack or its fabrics.

## The worker

One work queue, preemptive, below the cooperative HTTP server. It runs
`network_manager_process()` when a request or a network event wakes it and
once a second, which is what advances the confirmation deadline. Joining Wi-Fi,
a scan (up to ten seconds in the ESP-Hosted driver), the commit written to
settings and the resolver changes all happen there. Its stack is in SRAM: the
service is its own library, outside `app`, which is relocated to PSRAM.

## The adapter

Stable ids: the W5500 is found by its compatible and Wi-Fi as the station
interface, never by net_if index.

- **Addressing.** `configure()` stops DHCP, removes the IPv4 addresses and the
  gateway, and either starts DHCP or adds the static address, netmask and
  gateway. An interface whose settings did not change is left alone: restarting
  DHCP would take the address away from a browser using it. Disabling Ethernet
  takes the interface down; disabling Wi-Fi leaves the station interface up.
- **No DHCP of its own.** `CONFIG_WIFI_STA_AUTO_DHCPV4=n`: the driver would
  otherwise start DHCPv4 on every association.
- **Wi-Fi.** Joined through `NET_REQUEST_WIFI_CONNECT` (PSK for WPA2, SAE
  password for WPA3). Association, a failed join and a disconnect come from the
  driver's `net_mgmt` events; the callbacks set a flag and wake the worker. RSSI
  is refreshed by the worker every ten seconds, so status never waits on the
  coprocessor. A coprocessor that did not complete its initialisation — no
  firmware on the C6, as on board B — makes Wi-Fi "not present".
- **Scan.** The driver's `scan` operation is called directly with a callback of
  the adapter's. Through `net_mgmt` every result would travel as an event whose
  data is capped at `CONFIG_NET_MGMT_EVENT_INFO_DEFAULT_DATA_SIZE` (32 bytes),
  smaller than one `wifi_scan_result`.
- **DNS.** Manual: the servers DHCP and router advertisements added are removed
  from the resolver and the manual list installed (`DNS_SOURCE_MANUAL`);
  network-manager installs it again whenever a renewal replaces it. Automatic,
  or another manual list: the resolver context is closed, because
  `dns_resolve_remove_server_addresses()` matches no server when given no
  interfaces and a manual server has none (found on board B); servers the
  manual list displaced come back with the next lease renewal, and until then
  status shows none. `CONFIG_DNS_RESOLVER_MAX_SERVERS=4` gives each interface two;
  the API shows at most two.
- **Default route.** `net_if_set_default()` on the interface network-manager
  selects.

## Not covered in sim

Everything in `net_adapter.c` and `network_service.c` calls the network stack
or settings and is checked on the board (`docs/device-development/reports/p4`).
The policy those calls carry out is network-manager's and is covered there.

Open, recorded here as section 12 asks:

- Two interfaces on the same subnet: not investigated on hardware.
- A hidden SSID is joined by the coprocessor probing for it; not verified on a
  C6 (board B's has no firmware, owner's decision).
- Wi-Fi security modes in capabilities are this driver's (open, WPA2-PSK,
  WPA3-SAE), not queried from the installed coprocessor firmware, which offers
  no such call.
