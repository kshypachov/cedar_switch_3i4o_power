# Cedar web application

The browser interface of the switch, served by the device itself on port 80 and
talking to API v1 on the same origin. Russian only for now, with every string in
one dictionary.

Plan: section 4 of
[`development-plan.md`](../../../docs/device-development/development-plan.md).
Contract: [`api-contract.md`](../../../docs/device-development/api-contract.md) and
[`openapi.json`](../../../docs/device-development/openapi.json).

## Stack

React 19, TypeScript 5.9, Vite 8; vitest and Testing Library for unit and
component tests; Playwright for end-to-end. The owner left the choice open
(2026-09-13: flash is not the constraint, use what is best known); these were
chosen as the most familiar option that meets section 4's three constraints:

| Constraint | How it is met | Measured |
|---|---|---|
| `dist` ≤ 512 KiB gzip | Checked twice: by `scripts/stamp.mjs` after every build, and by the firmware build, which refuses to embed more | 80 164 bytes gzip (15 %) |
| reproducible from the lockfile, no network beyond the registry | `npm ci`; types generated from `openapi.json` in the repository, no network | — |
| no CDN, no external fonts | system fonts, assets as files; the device serves the page with `default-src 'self'`, and the e2e suite fails on any request leaving the origin or any console error | — |

`openapi-typescript` needs TypeScript 5, which is why it is not 7.

## Layout

```
src/api/        typed client over the generated schema: refusals, retries, jobs
src/state/      auth view, the one polling scheduler, the router, usePolling
src/i18n/       ru.ts - every string - and t()
src/components/ layout, cards, fields, error display, formatting
src/features/   auth (setup, login, access), device (overview), matter (window, codes, fabrics),
                network (status, forms, Wi-Fi scan, the apply transaction),
                logs (live tail, filters, pause, export, sources),
                coprocessor-update (ESP32 status, merged file, chunked upload, verify, UART write),
                system-update (STM32: running image and confirmation, MCUboot image upload,
                version comparison, install across the device's restart)
e2e/            Playwright, against the mock or the board
```

Decisions that are easy to undo by accident:

- **Types are generated, never written.** `npm run generate:api` writes
  `src/api/schema.gen.ts` from the document; it runs before every typecheck,
  test and build and is not committed, so a copy cannot go stale. A schema change
  that breaks a use fails the build.
- **At most two requests in flight per page.** The device serves four HTTP
  clients and the plan budgets for two browsers; a page that fanned out a
  request per card would lock the second browser out.
- **A bare 409 is retried.** Zephyr's server answers a second client on a
  resource that another client is mid-body on with 409 and no body, before any
  handler runs; nothing was executed, so repeating is safe for every method, and
  a mutation repeats with the same Idempotency-Key.
- **One polling scheduler**: a task never overlaps itself, runs every 5 s at
  most in a hidden tab, and stops for resources this firmware does not serve.
- **The password change keeps its Idempotency-Key** while the passwords are
  unchanged, so a retry after a lost response returns the same job. Its end is a
  401 on the job poll - the change revokes every session - which the page reads
  as success and asks for the new password.
- **No secrets in storage.** The session cookie is HttpOnly and the page keeps
  only the CSRF token, in memory.
- **Text is text.** SSIDs and other device strings are rendered by React, never
  as markup.
- **The QR code is drawn here, from the payload the device sends** (plan section
  6). `uqr` (MIT, a port of Nayuki's generator, 0.1.3 pinned) turns it into a
  module matrix and the page draws one SVG path; no markup is built from a
  string. The manual code and the PIN are separate fields, kept as strings so
  leading zeros survive.
- **Codes are asked for once per window**, when the window reports
  `codes_available`; they do not change while it stays open. A window opened
  by a controller shows no timer: the device does not know its timeout.
- **A window request keeps its Idempotency-Key** for every retry until its job
  finishes, like the password change.
- **The network screen keeps runtime and configuration apart**: what the
  interfaces do now (`network/status`) is one card, the committed configuration
  and its revision another, and the form is made once from the latter and then
  belongs to the person editing it. A committed change re-reads it.
- **An SSID is bytes.** The form sends back exactly the bytes it got from the
  configuration or a scan; only a typed SSID is encoded, as UTF-8, and its 32
  limit is counted in bytes. Bytes that are not UTF-8 are shown as U+FFFD.
- **The password is write-only**: `keep` when nothing is typed and SSID and
  security are unchanged, `replace` when typed, `clear` for an open network (and
  for a disabled Wi-Fi with nothing to keep); a protected profile change without
  a typed password is stopped at the password field before the device refuses
  it.
- **A transaction outlives the page.** It is reopened from `?txn=` or from
  `network/config.pending_transaction_id`, polled every second while it is
  pending and not at all once it has ended. After apply the transaction is
  polled, not the job, which parks at `waiting_confirmation`; confirm and
  rollback poll the job to its end. The countdown is the device's
  `remaining_seconds`, re-anchored on every poll.
- **Reconnect links carry the transaction** (`http://<address>/network?txn=<id>`):
  cookies and CSRF do not move to a new origin, so the person signs in there and
  confirms. Only http(s) URLs from `reconnect_urls` become links; an empty list
  (a DHCP address) is explained instead.
- **A refused confirm keeps the transaction open**: `409 invalid_state` means the
  new settings are not working yet; the confirm is retried under the same key
  until the deadline.
- **Wi-Fi is unavailable** when its interface carries `capability_unavailable`
  (the coprocessor is not ready) or a scan is refused with it: enabling and
  scanning are disabled, switching an enabled Wi-Fi off is not.
- **Logs are a live tail, not a history browser** (`features/logs`). The first
  request has no cursor and gets the newest records; each next request carries the
  cursor the device returned, every second on the shared scheduler, and again at
  once (up to five pages) while `has_more` says the device stopped early - by
  bytes or by its scan budget, which may leave a page empty. A filter change
  drops the cursor and the rows; so does `400 invalid_cursor`, which is not shown
  as an error. Text filters wait 400 ms after typing.
- **Pause stops the polling task**, not just the scrolling, so a paused page sends
  nothing; resume continues from the old cursor, and what the ring overwrote
  meanwhile arrives as the device's `gap`. Scrolling up stops following new rows
  until the button brings the view back.
- **A gap and a new boot are rows where they happened**; the page keeps 2000 rows
  and says when older ones were removed. Row keys are boot, source, kind and seq -
  seq restarts with a boot and a gap record may share one.
- **The export is a plain link** with the current filters: the browser downloads
  the attachment with the session cookie; nothing is buffered by the page.
- **Why ESP32 logs stop** comes from `logs/sources` (`reason`) and the UART's
  owner from `coprocessor/status.uart_mode`.
- **The ESP32 screen takes one kind of file** (`features/coprocessor-update`, P6):
  the merged `idf.py merge-bin` image, written from 0x0, which erases the
  C6's NVS - the screen says so before the file and asks again before the write.
  The install always sends `acknowledge_recovery: true`, and only after the
  checkbox that names what is replaced. OTA is shown as unavailable with the
  device's reason, never as a switch.
- **SHA-256 is computed here, in plain TypeScript** (`sha256.ts`): `crypto.subtle`
  exists only in a secure context, and the device is plain HTTP. The file is
  hashed in 256 KiB slices with a yield between them, so the page stays usable.
- **The next chunk goes only after the previous chunk's job succeeded**, and the
  offset is always the device's `received_bytes`. After a lost answer,
  `offset_mismatch` or `busy` the upload is read again; a retry of the same
  offset keeps its Idempotency-Key, a new offset gets a new one. Eight lost
  answers in a row stop the upload (`uploader.ts`).
- **An upload and an install survive a reload**: their ids are kept in
  `localStorage` (guarded; without storage nothing resumes and nothing breaks),
  the device is asked where they stand, and an id it no longer knows is dropped.
  Choosing the same file again (same size and SHA-256) continues the upload; a
  different file left on the device is never overwritten - it is offered for
  deletion.
- **Install phases are a list, not a percentage**: each phase is done, current,
  pending or stopped, and progress is shown for the current phase only, as the
  contract resets it per phase. Cancel is offered while the job says
  `cancellable`; a refused cancel (`invalid_state`) says the erase has begun.
  Polling carries on through lost answers and says the device is not answering;
  a job the device no longer knows (a reboot) points at `last_update`.
- **The STM32 screen follows the install across the device's own restart**
  (`features/system-update`, stage "Обновление STM32"). The upload is the ESP32
  one with `target: "stm32u585"` - same chunk loop, its own `localStorage` entry
  (`memoryAt`), so neither screen forgets the other's upload. The install job
  lives in the device's RAM and parks in `rebooting`: the page follows it that
  far, then polls `system/status` until `boot_id` changes (up to 180 s; MCUboot's
  swap takes ~40 s). A 401 on the way means the restart ended the session: the
  sign-in screen says so and the path stays `/firmware`, where `last_update`
  tells the outcome. The logic is in `install.ts`, away from React, on a fake
  clock.
- **An unconfirmed new firmware is a warning, not a detail**: a reset or power
  loss before confirmation returns the previous one, and the card says so with
  a countdown the page ticks between polls. While unconfirmed, upload and
  install are refused by the device and blocked on the page. An older image
  needs a checkbox, and only then is `acknowledge_downgrade: true` sent; the
  same version is a reinstall. The install asks once more in an inline panel.

## Commands

```sh
npm ci                  # exactly the lockfile
npm test                # generate types, vitest
npm run build           # typecheck, vite build, stamp dist/.cedar-build.json
npx playwright test     # e2e against the mock (serves dist with --ui)
npm run dev             # Vite with /api proxied to a mock on :8080

# once, before the firmware build embeds the page:
npm ci && npm run build
```

`tests/ci/run-frontend-tests.sh` runs the first four in order, as CI does.

The e2e suite uses the installed Chrome (`PW_CHANNEL=chrome`, the default), so it
downloads nothing; `PW_CHANNEL=` with `npx playwright install chromium` uses
Playwright's own browser, which is what CI does.

Against the board:

```sh
E2E_DEVICE_URL=http://192.168.88.14 E2E_DEVICE_PASSWORD=... npx playwright test
```

runs `e2e/device.spec.ts` only: sign in, overview, access, sign out, with the same
origin and console checks. It never touches the mock's control plane.

## Tests

- **Unit** (vitest): the client's refusal parsing, the bare-409 retry and its
  Idempotency-Key, the two-request limit, headers per operation; the scheduler's
  interval, no-overlap, background slowdown and abort; job polling; formatting.
- **Dictionary**: every error code of the device's table (read from the mock's
  transcription of `api_validation.c`), every field code, and every enum value
  the overview and the network screen show has Russian text; and a
  TypeScript-AST scan fails on text written into markup or readable attributes.
- **Fixtures**: typed by the generated schema, and validated against
  `openapi.json` with Ajv for what types cannot say. Request bodies the network
  screen builds are validated the same way (`src/test/schema.ts`).
- **Components**: setup, login, 429 with Retry-After, overview with SSIDs as
  text and unavailable resources, the session ending while polling, the password
  change with its job and key reuse; the network screen's runtime and committed
  cards, staging with the credential rules, 422 fields at their inputs,
  stale_revision and busy, the scan list (duplicate SSID, hidden, enterprise,
  truncated, markup and non-UTF-8 SSIDs, picking), Wi-Fi unavailable, apply →
  countdown → confirm, a refused confirm and its retry, timeout and explicit
  rollback, discard, `?txn=` and a missing transaction.
- **Unit** (network): SSID bytes, the candidate built from the form, the
  password action, JSON Pointer placement, reconnect links and `?txn=`.
- **Logs**: the controller (queries, cursor, gap and boot rows, unique keys,
  repeats, the row cap, filter change and invalid_cursor, uptime formatting,
  following) as units; the screen on OpenAPI fixtures - markup and ANSI-free
  text as text, markers and cut lines, polling from the cursor, has_more
  catch-up, filter change and debounce, invalid_cursor, reboot with gap, pause
  sending nothing, the USB-bridge reason, export links, 404 and 401; e2e against
  the mock (source filter, pause, both exports' headers) and a reboot answer
  injected with `page.route`, since the mock cannot reboot.
- **ESP32 update**: SHA-256 on the NIST vectors, a million "a", every padding
  boundary and a 1.4 MB input against Node, split inputs, slicing and abort; the
  chunk loop on a scripted device (order, keys, resume, a waiting job, lost
  answer, offset_mismatch, busy with and without a running job, a failed job,
  a refusal, the bounded and the in-a-row loss count); phases; storage without
  storage. The screen on OpenAPI fixtures: status and OTA reason, interrupted
  with recovery, bridge busy, 404, the local size refusal, the whole upload
  with raw chunk bodies checked against the file, resume, a vanished upload, a
  foreign file and its deletion, busy, a failed verification, the install with
  the acknowledgement and every phase, cancel refused, a failed write,
  ethernet_required, lost answers, a job gone after a reboot, 401, text as text.
  e2e against the mock: upload → verify → write → running module, bare app
  refused, Wi-Fi refused, USB bridge, a reboot mid-write, a reload mid-upload
  and deletion.
- **STM32 update**: version parsing and order; following the job (rebooting,
  silence only after `requesting`, 404, 401) and waiting for a new `boot_id` on
  a fake clock (silence, timeout, 401); the countdown; phases and the separate
  storage entry. The screen on OpenAPI fixtures: confirmed, unconfirmed with
  warning and countdown, rolled back, reasons, the size limit, the whole path
  with `target` and the restart, the declined confirmation, downgrade, reinstall,
  a rejected image, refusals of upload and install, a failed job, the session
  ending with the restart, reload resume, an ESP32 upload not taken, a job gone,
  the overview link. e2e against the mock with real MCUboot images built in the
  test: install through the restart to self-confirmation, a reset that rolls
  back, downgrade, garbage and a damaged TLV hash, refusal while unconfirmed,
  a reload mid-upload.
- **End-to-end** (Playwright, against the mock): setup from the page, the
  policy refusal, sign in / reload / sign out with the cookie's flags, the
  password change, two browsers at once; the network change applied and
  confirmed, a refused gateway at its field, scan and pick, the timeout rollback
  (`/__mock/advance`), a reconnect link opened in a second browser and rolled
  back there, a confirm refused while `network_health=unhealthy`, and Wi-Fi with
  the coprocessor offline. Each also checks no request left the origin and the
  console stayed clean. One run against the board.

The suites were checked with mutations; counts are in the P2 report.
