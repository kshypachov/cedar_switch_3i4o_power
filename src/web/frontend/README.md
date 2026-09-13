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
src/features/   auth (setup, login, access), device (overview), matter (window, codes, fabrics)
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
  the overview shows has Russian text; and a TypeScript-AST scan fails on text
  written into markup or readable attributes.
- **Fixtures**: typed by the generated schema, and validated against
  `openapi.json` with Ajv for what types cannot say.
- **Components**: setup, login, 429 with Retry-After, overview with SSIDs as
  text and unavailable resources, the session ending while polling, the password
  change with its job and key reuse.
- **End-to-end** (Playwright, against the mock): setup from the page, the
  policy refusal, sign in / reload / sign out with the cookie's flags, the
  password change, two browsers at once; each also checks no request left the
  origin and the console stayed clean. One run against the board.

The suites were checked with mutations; counts are in the P2 report.
