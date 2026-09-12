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
     web-api  ──────────────────────────────────┐
        │ stage / apply / confirm / rollback    │ 202 + job id
        ▼                                       ▼
  network-manager ──── validation ──────▶ api-validation
        │  │                                (codes, field paths, parsers)
        │  └──────── job identity ────────▶ job-manager
        │
        ├──── generations ────────────────▶ device-config-store
        │                                (committed / pending, secrets)
        └──── configure / connect / scan ─▶ interface adapter
                                              (fake in sim, W5500 + C6 on the board)
```

This module is where the policy lives. `device-config-store` knows how to make
a generation durable but not whether a gateway belongs on this subnet;
`api-validation` knows that an address is or is not a usable host but not which
interface has to stay reachable. Those decisions need context, and they are
here.

It owns the timers too. The store deliberately holds none — its correctness
must not depend on anything still running.

## The transaction

```
  stage ──▶ STAGED ──apply──▶ APPLYING ──execute──▶ AWAITING_CONFIRMATION
              │                   │                        │
              │ 300 s             │ adapter failed         │ confirm
              ▼                   ▼                        ▼
           EXPIRED             FAILED                  COMMITTED
              
      any of STAGED / APPLYING / AWAITING ──rollback or deadline──▶ ROLLED_BACK
```

Exactly one exists at a time. A second is refused rather than replacing one
another administrator is waiting on — and losing which change is in flight
would leave a rollback with nothing definite to roll back to.

**Applying is two calls on purpose.** `network_apply()` validates, journals and
returns so the caller can send its `202`; `network_apply_execute()` is what the
worker runs afterwards and is the first thing that touches an interface. The
contract requires the journal to be durable and the response to be sent before
the network changes, and one call could not do both.

The confirmation deadline is armed at `network_apply()`, not after the
interfaces change. A worker that never runs, or dies half way, then still ends
in a rollback instead of leaving the device on an unconfirmed configuration
forever.

## What "healthy" means

Confirm is refused until every *enabled* interface has a link, an address, and
a route if the configuration asked for a gateway. Not a ping to a public host:
that would make a working LAN look broken whenever the internet is down.

A client reaching the confirm endpoint proves its own path works and nothing
else. The contract is explicit that a browser on an unchanged Ethernet says
nothing about whether Wi-Fi actually joined, so both interfaces are checked.

An unhealthy transaction stays awaiting rather than failing — DHCP may simply
not have finished — and the deadline decides when waiting has gone on too long.

## The adapter

Everything touching hardware is behind `struct network_iface_ops`. Two rules
bind an implementation:

- **It makes no policy decisions.** In particular it must not start DHCP by
  itself. Section 5 is explicit that one service owns DHCP, DNS and Wi-Fi
  credentials together, and an adapter that starts DHCP whenever it associates
  makes static addressing impossible to express. The existing Wi-Fi adapter
  does exactly this today; moving it is part of wiring this module to the
  board.
- **It must not call back into network-manager.** Callbacks run with the
  module's mutex held.

## Secrets

The input type carries only the Wi-Fi credential. A network transaction has no
field for the admin password and therefore structurally cannot change it; the
update handed to the store marks it `keep`, which is what makes that true
rather than merely intended. The Wi-Fi password reaches the radio and the
store, and appears in nothing this module hands back.

## Lifecycle and threads

No threads and no I/O of its own. Time comes from an injected clock,
interfaces from an injected ops table. `network_manager_tick()` advances the
deadlines and is called from the worker at least once a second. No dynamic
allocation.

## Testing

`tests/network_manager` covers the four things section 12 asks of this tier —
the full transaction to committed and to rolled_back, IPv4/mask/DNS validation,
the confirmation timer, and the health policy over fake interfaces — plus the
credential rules, the status snapshot and scanning. 51 tests, all in sim.

The suite was checked against eighteen mutations of the implementation. It
caught seventeen; the one that survived was the health check's route
requirement, which nothing exercised because every fake interface with an
address also had a route. The fake grew a `suppress_route` flag and the case
was added. Three further mutations first "failed" only by not compiling, which
is not the suite catching anything, and were reformulated until they built.

Run it:

```sh
tests/ci/run-sim-tests.sh -s cedar.network_manager
```

### Not covered in sim

Recorded here as section 12 requires:

- **Real interfaces.** DHCP and static on the W5500 and the C6, pulling the
  cable, reconnect, and a real scan are the hardware tier's. A fake that
  settles an address the moment it is configured is a model of an interface,
  not one.
- **Two interfaces on the same subnet.** Section 5 asks for this to be
  investigated on hardware before a rule is written, so nothing here rejects
  it. When the behaviour is known, the check belongs in `network_validate.c`.
- **The reconnect hints** the contract asks the UI to show after an address
  change. They are derived from the candidate by the web layer, which does not
  exist yet.
