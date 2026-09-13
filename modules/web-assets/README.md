# web-assets

The browser application, served from flash: lookup, MIME, ETag, gzip, cache
policy, and the split between SPA navigation and a plain 404.

Contract: "Общие правила" in
[`api-contract.md`](../../docs/device-development/api-contract.md) (hashed assets
may be cached, the page revalidates, an unknown API URL is a JSON 404 and never
the page) and section 4 of the plan (no CDN, works offline, 512 KiB gzip). The
header `include/web_assets/web_assets.h` carries the policy.

## Two halves

**Build time** — `tools/web-assets/gen_web_assets.py` reads the frontend's
`dist/` and writes a C table (`web_assets_data.c` in the build directory) and a
JSON manifest. It decides everything that does not depend on the request: MIME
from a closed list (an unknown extension fails the build), gzip only when it
saves a tenth and the file is at least 1 KiB, a strong ETag over the stored
bytes, immutable caching only for Vite's hashed names under `assets/`. Output is
reproducible, and an unchanged table is not rewritten. The application's
CMakeLists runs it at build time with `--budget-kib 512`.

**Run time** — `web_assets_respond()` answers a non-API request: GET only;
`/` and extension-less paths outside `assets/` get the page; a gzip-only file to
a client that does not accept gzip is 406; a matching `If-None-Match` is 304
with `Connection: close`; the page carries a `default-src 'self'` policy, no
referrer and frame denial; everything carries `nosniff`.

Zephyr's own static resources were not used: they send no ETag, no
Cache-Control and no security headers. The fallback resource in
`src/web/web_server.c` sends every non-API path here.

## The mock serves the same page

`python -m cedar_contract.mock --ui <dist>` serves a built frontend with this
policy: it runs the same generator, reads the CSP literal out of `web_assets.c`,
and transcribes `web_assets_respond()` (`mock/static.py`). The frontend's
end-to-end tests therefore meet the headers the device sends, CSP included.

## Budget

The first real build (P2): 4 files, 257 697 bytes, 80 373 bytes stored
(JavaScript and CSS gzipped), 15 % of the 512 KiB budget. Firmware FLASH use
went from 27.7 % to 29.6 %.

## Testing

- `tests/web_assets` (sim, 9 cases): lookup, Accept-Encoding parsing with q
  values, weak ETag comparison, every response branch with its exact headers, and
  the largest header set fitting.
- `tests/web_api_http` (sim): a table generated from a fixture dist, through
  Zephyr's server.
- `tools/web-assets/tests` (pytest, 14): the generator's decisions, reproducibility,
  refusals, the budget, and that the CSP the mock reads is the C literal.
- `tools/api-contract/tests/test_mock_static.py`: the transcription, case by case.

```sh
tests/ci/run-sim-tests.sh -s cedar.web_assets
cd tools/web-assets && python -m pytest tests
```
