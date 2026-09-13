# web-auth

The single local administrator of the web interface: first-time setup, sign-in,
sessions with a CSRF token, the password change, and a limit on guessing.

Contract: "Сессия и устройство" in
[`api-contract.md`](../../docs/device-development/api-contract.md), the
`AuthState`, `SetupRequest`, `LoginRequest`, `Session` and `PasswordRequest`
schemas in [`openapi.json`](../../docs/device-development/openapi.json), and
section 9 of the development plan. The public header carries the design; this
file covers how the module fits the system and what was measured.

## What it is

One administrator named `admin`. The password is stored only as a
PBKDF2-HMAC-SHA256 verifier (`"CWA1"`, KDF id, iteration count, 16-byte salt,
32-byte key — 60 bytes) in settings-registry under `auth/admin_verifier`. A
device without a verifier is in setup: `GET /auth/state` publishes a random
setup token (owner's decision: the token is shown in the web interface), and
the first `POST /auth/setup` presenting it with a password becomes the
administrator. Sessions are 32 random bytes in RAM with their own CSRF token,
a 30-minute idle and 8-hour absolute lifetime; a reboot ends them all.

The module has no HTTP in it. `web-api` maps its verdicts onto the error table,
and `src/web/api/v1/auth.c` binds the operations.

## Decisions worth knowing before changing it

- **What the setup token proves.** Nothing about physical access — whoever opens
  the page first on the network sets the password. It does tie setup to a client
  that read this device's own `/auth/state`, which a cross-site form or a script
  firing blind cannot. That only holds together with `web-api`'s Host check (a
  DNS-rebinding page would otherwise read the token as same-origin) and its
  Origin check. Recorded in section 13 of the plan as an accepted risk.
- **Setup is atomic by claim.** A second request during the first one's
  derivation is told `busy`; a failed store reopens setup with the same token.
- **Guessing is limited twice**: per client address (five free failures, then a
  doubling wait up to 300 s) and in total (30 failures per 5 minutes). The
  per-address limit alone is defeated by address rotation on IPv6; the global
  one also delays the real administrator during a flood, chosen over an
  unbounded guessing rate on a trusted network.
- **A damaged verifier locks sign-in instead of reopening setup.** There is no
  recovery path in the first version: no factory reset, and the settings shell is
  not built in. That is an open question for the owner (plan section 13), not a
  solved one.
- **Why not device-config-store.** It has a slot named
  `DEVICE_CONFIG_SECRET_ADMIN_PASSWORD`, but that store versions the network
  configuration as one generation with one journalled transaction at a time. A
  password change stored there would bump the network revision (turning an
  unrelated staged change into `409 stale_revision`) and would be refused while
  a network change awaits confirmation. The slot is unused; whether to remove it
  is left to P4, when that store's layout is final.

## Threads, and the derivation

The core takes a mutex around its state and calls the platform functions
without it. Setup, login and the check of a current password call the key
derivation on the caller's thread — and that caller is Zephyr's HTTP server
thread, which is **cooperative** (`K_PRIO_COOP(NUM_COOP_PRIORITIES - 1)`,
hard-coded in `http_server_core.c`).

Measured on the board (P2 report, `hw/logs/02` and `03`): with the derivation
computed directly on that thread, 10000 iterations did not finish in 18 s, and
for the whole time the network stack's own threads, Matter and every probe from
a second client stood still. So the production adapter
(`lib/web_auth_psa.c`) hands the derivation to a preemptive thread at priority
14 whose stack is in SRAM, and the HTTP thread waits on a semaphore — which
blocks it and lets everything else run. Measured again (`hw/logs/04`): during a
3.5 s login the shell answered 4 of 4 probes within 16 ms and ping lost nothing.

The HTTP server itself still answers nobody else while it waits, so the
iteration count is a latency budget:

| Iterations | `web_auth kdf` on the board | Login over HTTP |
|---:|---:|---:|
| 100 | 33 ms | — |
| 3000 | 943 ms | 1.12 s (default) |
| 10000 | ≈3.1 s | 3.41 s |

PSA Crypto's PBKDF2 re-keys HMAC on every iteration, the build is `-Og`, and
code executes in place from QSPI; a derivation with precomputed pads would be
roughly twice as fast, and is the option if the budget must grow. The verifier
carries its iteration count, so changing the default affects only passwords set
afterwards.

`libweb_auth.a` is deliberately not in `src/helpers/psram_sections.ld`: its
`.bss` is 2.3 KB and the derivation stack should be in the faster memory.
Measured stack high-water: `web_auth_kdf` 1624 of 4096 bytes.

The firmware sets `CONFIG_WEB_AUTH_KDF_THREAD_PRIORITY=13`, one level above the
Matter thread (P3): at the default 14 the two shared a priority with time
slicing off. This did not remove the symptom it was meant for - on the board a
sign-in still sometimes took more than 10 s while Matter spent seconds in
storage work, with ping and the shell answering. The cause is open
(`docs/device-development/reports/p3/STATUS.md`).

`web_auth kdf [iterations]` on the shell times one derivation on the production
path (`CONFIG_WEB_AUTH_SHELL`). It touches no stored credential.

## Testing

`tests/web_auth` — 48 cases in the sim tier with every platform function faked:
the clock moves only when a test moves it, and the fake derivation can call back
into the module, which is how the concurrent setup, the login that races a
password change, and the claim are tested without threads. Covered: init with a
fresh, valid, failing and each kind of damaged store; setup with right, wrong,
missing and prefix tokens and tokens with anything appended, the password policy in code points, storage and
derivation failures; login; both rate limits including slot eviction; idle and
absolute expiry, the least-recently-used eviction, tombstones; CSRF; the
password change and each of its failures.

The PSA and settings-registry adapters have no sim suite: PSA Crypto and a
settings backend are not in the CI sim job. They are covered on the board
(`docs/device-development/reports/p2/hw`): setup, login, rate limit, password
change, and persistence across a reset (`hw/logs/08`).

```sh
tests/ci/run-sim-tests.sh -s cedar.web_auth
```

The suite was checked with mutations of the core; the counts are in the P2
report.
