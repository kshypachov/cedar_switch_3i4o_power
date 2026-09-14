# zephyr-log-source

The Zephyr log backend that makes the STM32's own log messages into log-store
records.

Contract: sections 3 and 7 of `docs/device-development/development-plan.md`.
The public header carries the rules; this file covers how the module fits the
system.

## How it fits

```
LOG_INF() ─▶ deferred buffer (CONFIG_LOG_BUFFER_SIZE) ─▶ log thread
                                                          ├─▶ shell backend (USART1, telnet)
                                                          └─▶ this backend ─▶ log_store_append()
```

It is one more backend beside the shell's; the shell and telnet backends are
untouched. It never writes to HTTP or to flash: the web API reads the store.

## Lifecycle

- `CONFIG_ZEPHYR_LOG_SOURCE_INIT_STORE` (default y) initialises log-store at
  `POST_KERNEL` priority 99, before any thread runs.
- The backend is autostart and reports not ready (`-EBUSY`) until the store is
  ready. On the board the log thread finds it ready on its first pass, so every
  message logged during boot - still waiting in the core's deferred buffer - is
  delivered to it. Only what overflowed that buffer is lost, and counted.
  Without a processing thread (the sim suite) a backend that was not ready at the
  core's init is never activated by the core; the suite enables it itself.
- After `log_panic()` it stops touching the store and only counts.

## Threads and ownership

`process()` runs on the log thread; the formatting buffers are static and used
only there (the log thread's stack is 2 KiB on the board). `dropped()` runs on
the log thread; `panic()` and `process()` after it may run in a fault handler,
where only atomics and log-store's loss spinlock are touched. Counters are
atomics, readable from any thread.

## Errors

Nothing is reported by logging - the module contains no `LOG_*` call. Every loss
is `LOG_STORE_LOSS_BACKEND` for the STM32 in the store (that is `dropped_count`
in the API) and one of the counters in `struct zephyr_log_source_stats`:
dropped by the core, refused by the store, after the panic.

## A Zephyr log core detail that matters here

`z_impl_log_process()` (`subsys/logging/log_core.c`) reports drops to backends
only when `k_uptime_get() - last_failure_report > CONFIG_LOG_FAILURE_REPORT_PERIOD`,
and adds the period to `last_failure_report` on **every** call, whether it
reported or not. After N processed messages a report needs an uptime of N
periods: with the default 1000 ms and a few thousand messages at boot, `dropped()`
effectively never arrives again. `CONFIG_LOG_FAILURE_REPORT_PERIOD=0` makes the
comparison true on every call; the sim suite runs with it, and the firmware needs
it for `dropped_count` to mean anything.

## Tests

`tests/zephyr_log_source` (sim, deferred core without a thread, real log-store):
not ready before the store; text without prefix, all four levels, module, the
time of logging; printk-style messages with null level and module; a hexdump's
lines; control characters and bad UTF-8; the 512-byte cut on a character
boundary and past the raw buffer; drops counted exactly (stored + dropped = sent);
no recursion; panic. Hardware tier: shell and telnet output unchanged beside it,
and a burst from `log enable dbg` reaching the store.
