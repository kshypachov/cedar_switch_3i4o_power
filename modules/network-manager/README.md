# network-manager

The transaction that changes this device's network without being able to lose
it.

Contract: section 5 of `docs/device-development/development-plan.md` and the
"Сеть" section of `docs/device-development/api-contract.md`. The public header
carries the full API documentation; this file covers only how the module fits
the system.

## Why it exists

The only way to manage this device is over the network, so a change that goes
wrong takes away the means of undoing it. A wrong address, a Wi-Fi password
that does not work, a gateway on another subnet — each turns a five-second edit
into a service visit.

Three rules follow, and they are the whole module:

1. **Check before touching anything.** Validation runs while the device is
   still reachable and nothing has changed, and every rejection names the field
   that is wrong.
2. **Record before changing.** The pending generation is durable and verified
   before the first interface is configured. A change nothing recorded is a
   change nothing can undo.
3. **Silence is a rollback.** The change is not kept until someone confirms
   from the other side that they can still reach the device. If nobody does,
   the deadline puts it back.

## How the pieces fit

```
     web-api (src/web/api/v1/network.c)
        │ stage / apply / confirm / rollback / scan      202 + job id
        ▼                                                 ▲
  network-manager ──── validation ──────▶ api-validation  │
        │  │                                (codes, field paths, parsers)
        │  └──────── job identity ────────▶ job-manager ──┘
        │
        ├──── generations ────────────────▶ device-config-store
        │                                (committed / pending, secrets;
        │                                 on the board in settings-registry)
        └──── configure / connect / scan ─▶ interface adapter
                   ▲                         (fake in sim; src/services/network
                   │                          on the board: W5500 + ESP-Hosted)
     network_manager_process()
     on the service's one worker thread
```

This module is where the policy lives. `device-config-store` knows how to make
a generation durable but not whether a gateway belongs on this subnet;
`api-validation` knows that an address is or is not a usable host but not which
interface has to stay reachable; the adapter knows how to start DHCP but not
when. Those decisions need context, and they are here.

It owns the timers too. The store deliberately holds none — its correctness
must not depend on anything still running.

## Two halves: deciding and doing

Every request is two calls. The first — `network_stage_config()`,
`network_apply()`, `network_confirm()`, `network_rollback()`,
`network_scan_begin()` — checks, records the decision under the mutex and
returns; it never touches an interface. `network_manager_process()` does what
was recorded: it copies what it needs, lets go of the mutex for the adapter
call, and takes it again to record the outcome, checking that the state it
started from still holds.

The reason is the HTTP server. Its one thread is cooperative; a Wi-Fi
association or a ten-second scan run from a handler would stop every other
request for as long (the password derivation proved this in P2). And the
contract requires the journal to be durable and the `202` sent before the
network changes, which one call could not do.

P1 split apply this way. P4 split confirm (the commit is written on the worker,
as the contract says), rollback and scan too, because on the board each of them
blocks on the coprocessor or on settings.

A request that arrives while `process()` works is simply the next thing it
does: a rollback asked for during a push lets the push finish and then undoes
it, on the same job.

## The transaction

```
  stage ──▶ STAGED ──apply──▶ APPLYING ──process──▶ AWAITING_CONFIRMATION
              │   │               │                  │        │
              │   │ journal       │ adapter refused  │        │ confirm → process
        300 s │   │ failed        ▼                  │        ▼
              ▼   ▼          ROLLING_BACK ◀─ DELETE ─┤    COMMITTED
          EXPIRED FAILED          │         deadline ┘
                                  │ process
                                  ▼
                ROLLED_BACK (asked for, or timed out: resource_expired)
                FAILED      (the push or the commit failed; restored first)

  STAGED ──DELETE──▶ ROLLED_BACK (on a network_discard job of its own)
```

Exactly one exists at a time. A second is refused rather than replacing one
another administrator is waiting on — and losing which change is in flight
would leave a rollback with nothing definite to roll back to.

The confirmation deadline is armed at `network_apply()`, not after the
interfaces change. A worker that never runs, or dies half way, then still ends
in a rollback instead of leaving the device on an unconfirmed configuration
forever. `remaining_seconds` counts it down while applying too, rounded down.

A confirm accepted before the deadline is honoured even if the worker only
writes the commit after it. Once accepted, the change can no longer be rolled
back.

## What the mock and the device share

The frontend is built against the mock server
(`tools/api-contract/cedar_contract/mock/network.py`), so the device answers
the same way: field paths into the request body (`/config/...`), the same
codes, and — because the error keeps the first six fields — the same order.
A timed-out confirmation ends `resource_expired` (the mock's DECISION); a
rollback somebody asked for ends with its job `succeeded`.

Where the device knows something the mock could not, P4 brought the mock to the
device: the deadline starting at apply, the host rule of
`api_ipv4_is_usable_host()` for addresses and gateways, an unspecified resolver
refused by index, Wi-Fi refused while the coprocessor is not ready, a confirm
refused while the interfaces are not working, a scan refused during a change
and only the latest scan keeping its results. `tools/api-contract/README.md`
lists them. One rule stays device-only because the mock's links are always up:
at least one enabled interface must have a link when staging.

## What "healthy" means

Confirm is refused until every *enabled* interface has a link, an address, and
a route if the configuration asked for a gateway. Not a ping to a public host:
that would make a working LAN look broken whenever the internet is down.

A client reaching the confirm endpoint proves its own path works and nothing
else. The contract is explicit that a browser on an unchanged Ethernet says
nothing about whether Wi-Fi actually joined, so both interfaces are checked.

An unhealthy transaction stays awaiting rather than failing — DHCP may simply
not have finished — and the deadline decides when waiting has gone on too long.

## Policies kept by the worker

`process()` also keeps three things true between transactions:

- **Default route**: the preferred interface while it has a link and an
  address, otherwise the other one if that does (section 5: Ethernet preferred,
  Wi-Fi fallback). Set through the adapter only when the choice changes.
- **Manual resolvers stay manual**: when the resolver list in force differs
  from a manual configuration — a DHCP renewal replaced it — it is installed
  again.
- **Wi-Fi rejoins**: an enabled, present radio that is neither associated nor
  connecting is asked to join again, five seconds after it dropped and then
  twice as late each time, up to a minute.

## The coprocessor's other owners

The ESP32-C6 that carries Wi-Fi also has a UART, which coprocessor-manager
hands to a USB bridge or a flasher (P5). Plan section 3 makes an apply
exclusive with those and with a manual reset of the chip, and a scan exclusive
with flashing. Two optional hooks in `struct network_iface_ops` carry this
module's half; the board adapter maps them to `coprocessor_manager_claim()` and
`coprocessor_manager_release()`, and this module never includes that header.

- **Claim-then-check.** An apply claims once it is otherwise acceptable (the
  transaction exists, is staged, the timeout is in range) and before its job or
  journal exists; a scan claims after the "change in progress" and "scan
  running" checks. coprocessor-manager marks a switch or a reset first and then
  reads the claims, so of two conflicting operations at most one proceeds.
- **A refusal is `409 busy` and leaves nothing behind**: the candidate stays
  `staged`, there is no job under the Idempotency-Key and no journal, so a retry
  once the UART is back is a new request, not a replay of the refusal. A request
  that is wrong for another reason gets that reason; the claim is not asked.
- **Given back on every end.** An apply's claim is released in `finish()` —
  committed, rolled back on request or by timeout, failed on the adapter, the
  commit or the journal — and on the job-creation paths that create nothing. A
  scan's is released when its results are in, when it fails, or when an apply
  accepted before it ran overtakes it. A replay by key answers before the claim,
  so it never claims twice. `network_manager_init()` releases whatever the
  previous run held.
- The hooks come as a pair or not at all (`-EINVAL`); without them everything
  is granted, which is the sim tier's and P4's behaviour. They run with the
  mutex held and must not block.
- `network_manager_restore_defaults()` does not claim: it runs at boot, from the
  five-short-boots rule, before a bridge or a flasher can exist.

## Status

`network_get_status()` reports each interface as the adapter observes it —
link, addresses of both families with their source, gateway and route, MAC,
association and RSSI — plus the configuration's `enabled` and a derived
`state` (`disabled`, `down`, `connecting`, `addressing`, `ready`, `failed`).
`connected` is never produced: an associated interface waiting for DHCP is
`addressing`. An absent coprocessor is `failed`, and the web layer says why.

## Boot and the physical recovery

`network_manager_boot()` takes device-config-store's recovery report. The next
`process()` puts the committed configuration on the interfaces — every boot's
first network configuration comes from here — and a transaction the store
rolled back is remembered, so a client polling it gets 410 `boot_changed`
instead of 404.

`network_manager_restore_defaults()` is the owner's five-power-cycle recovery
(the counting lives in `src/services/network/boot_streak.c`): Ethernet on DHCP,
automatic DNS, Wi-Fi disabled with its password cleared but its SSID, security
and hidden flag kept. The administrator is carried forward untouched.

## The adapter

Everything touching hardware is behind `struct network_iface_ops`. Three rules
bind an implementation:

- **It makes no policy decisions.** In particular it must not start DHCP by
  itself. Section 5 is explicit that one service owns DHCP, DNS and Wi-Fi
  credentials together, and an adapter that starts DHCP whenever it associates
  makes static addressing impossible to express. On the board this is also why
  `CONFIG_WIFI_STA_AUTO_DHCPV4=n`.
- **It must not call back into network-manager.**
- **`get_status` and `get_dns` must not block**: they run under the mutex, on
  whichever thread asked — usually the HTTP server's. Everything else is called
  only from `process()`, without the mutex.

## Secrets

The input type carries only the Wi-Fi credential. A network transaction has no
field for the admin password and therefore structurally cannot change it; the
update handed to the store marks it `keep`, which is what makes that true
rather than merely intended. The Wi-Fi password reaches the radio and the
store, and appears in nothing this module hands back.

## Lifecycle and threads

No threads and no I/O of its own. Time comes from an injected clock,
interfaces from an injected ops table. No dynamic allocation.

## Testing

`tests/network_manager` covers the four things section 12 asks of this tier —
the full transaction to committed and to rolled_back, IPv4/mask/DNS
validation, the confirmation timer, and the health policy over fake interfaces
— plus what P4 added: the worker's half and requests arriving while it works,
field paths and codes and their order as the mock produces them, the status and
its derived state, the default-route, resolver and rejoin policies, boot and
the factory restore. The interface fake (`tests/fakes/fake_iface.c`) is shared
with `tests/web_api`, which drives the HTTP bindings over the same module.

Run it:

```sh
tests/ci/run-sim-tests.sh -s cedar.network_manager
```

### Not covered in sim

Recorded here as section 12 requires:

- **Real interfaces.** DHCP and static on the W5500 and the C6, pulling the
  cable, reconnect, and a real scan are the hardware tier's
  (`docs/device-development/reports/p4`). A fake that settles an address the
  moment it is configured is a model of an interface, not one.
- **Two interfaces on the same subnet.** Section 5 asks for this to be
  investigated on hardware before a rule is written, so nothing here rejects
  it. When the behaviour is known, the check belongs in `network_validate.c`.
