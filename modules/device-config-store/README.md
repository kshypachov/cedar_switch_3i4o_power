# device-config-store

The durable, versioned, transactional home of everything the device must
remember across a reboot: network configuration and the secrets that go with
it.

Contract: section 5 of `docs/device-development/development-plan.md` and the
"Сеть" section of `docs/device-development/api-contract.md`. The public header
carries the full API documentation; this file covers only how the module fits
the system.

## Why it exists

`settings-registry` stores typed keys and tells interested code when one
changes. That is the wrong shape for a network change, for two reasons the
contract makes explicit.

**A configuration change is a transaction, not a field write.** Applying an
address, a gateway, a DNS policy and a Wi-Fi password one key at a time has a
window in which the device holds half of the old configuration and half of the
new one. Lose power there and it comes up unreachable, which on a device whose
only management interface is the network it just broke is unrecoverable without
a service visit.

**The change has to be undoable by a reboot.** Section 5 requires the pending
snapshot to be written durably *before* the network is touched, so that a
device which never receives its confirmation returns to the configuration that
was working. Nothing key-based can express that.

## The generations

```
   begin()                   commit()
     │                          │
     ▼                          ▼
  pending  ──────────────▶  committed
     │        confirm           ▲
     │                          │
     └──▶ rollback() ───────────┘
          or a reboot
```

`committed` is the configuration in force and the one a reboot returns to.
`pending` is journalled and applied but unconfirmed. There is never more than
one of each — a second `begin()` is refused rather than silently replacing a
transaction another administrator is waiting on.

The RAM candidate the contract keeps for 300 seconds before apply is *not* here.
That belongs to `network-manager`, which validates it; this module is reached
only at apply time, when the decision to change the network has been made and
the intent has to become durable.

## Surviving power loss

Three slots. Two hold the committed generation and are written alternately, so
a commit never overwrites the only good copy; the third holds the journal. Each
record carries a CRC over its header and another over its payload, and the
newest record that passes both wins.

That gives a defined answer at every instant a commit can be interrupted:

| Power lost | On the next boot |
|---|---|
| During the journal write | Journal fails its CRC, is discarded, committed generation stands |
| After the journal, before the commit | Journal describes a higher revision than committed — rolled back, and the transaction id is reported |
| During the commit write | Torn slot fails its CRC, the other slot still holds the older generation, journal rolls back |
| After the commit, before the journal is erased | Journal describes a revision that is already committed — the commit had succeeded, so it stands and only the leftover is cleaned up |

That last row is why `device_config_pending_commit()` returns success even when
erasing the journal fails: the new generation is durable and in force, and
reporting a failure would make a client undo a change that actually took.

Every write is read back and compared before it counts. Section 5 requires the
pending snapshot to be verified before the network is changed, and the same
check on commit is what stops a silently failed write from being reported as a
committed configuration.

## Secrets

Secret bytes are not members of `struct device_config`. A snapshot handed to an
HTTP handler carries `password_set` and nothing else, so it cannot leak a
password however the handler is written — the bytes are not in the struct it
holds. `device_config_secret_get()` is the only way to reach them, and a caller
has to ask on purpose.

The two flags are *derived* from the secrets that survived an update, never
taken from the caller. A handler cannot advertise a password that is not stored,
nor lose one by forgetting to set a flag.

`keep`/`replace`/`clear` mirror the contract's `CredentialChange.action`, and
`keep` is the zero value so a zero-initialised update changes no secret at all.
That is the contract's rule that an absent field must not clear a credential,
made structural: the mistake requires writing code, not omitting it.

## Schema

The struct layout *is* the persisted schema, so `BUILD_ASSERT` pins the size of
every persisted struct. Changing one without bumping
`DEVICE_CONFIG_SCHEMA_VERSION` fails the build rather than misreading records on
a device already in service.

A record from an older schema is migrated on load. A record from a newer one is
refused, because a downgrade guessing at a layout it has never seen is how a
device loses its settings. Refused means falling back to factory defaults rather
than declining to boot: Ethernet DHCP keeps the device reachable for someone to
fix it, and an unreachable device cannot be fixed at all. `init()` reports that
case separately from a genuinely blank device, because they need very different
responses.

## Lifecycle and threads

No threads of its own. Both generations are cached in RAM with storage as their
durable mirror, so a `GET` never touches flash. Every entry point takes an
internal mutex and snapshots are copied out under it. Not ISR-safe. No dynamic
allocation.

## Testing

`tests/device_config_store` covers the transaction, both reboot branches, torn
and failing writes, corrupt and lost slots, the schema gate, secret handling and
the wire names — 33 tests, entirely in the sim tier. The storage backend is
injected, so power loss is produced on demand rather than staged on a bench
supply. The suite was checked against ten mutations of the implementation, each
of which it caught.

Run it:

```sh
tests/ci/run-sim-tests.sh -s cedar.device_config_store
```

### Not covered in sim

Two things, both recorded here as section 12 requires:

- **Migration execution.** Version 1 is the only schema that has shipped, so the
  migration table is empty and only the dispatcher's rejection paths — a newer
  version, and an older one with no path to the present — are exercised. The
  first real migration must arrive with its own fixture of a genuine
  older-version record.
- **Real flash behaviour.** A whole-slot write over LittleFS or NVS is not the
  same thing as the fake's `memcpy`, and neither is a partial erase. The
  hardware tier owns this: the plan's table asks for real NVS/LittleFS and power
  loss at commit on the board. Passing sim says the state machine is right, not
  that the backend is.
