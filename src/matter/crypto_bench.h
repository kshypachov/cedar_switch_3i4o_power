#ifndef CEDAR_SWITCH_3IN4OUT_POWER_CRYPTO_BENCH_H
#define CEDAR_SWITCH_3IN4OUT_POWER_CRYPTO_BENCH_H

#include <stddef.h>

struct shell;

int matter_crypto_bench_run(const struct shell *sh, size_t iterations);

#endif // CEDAR_SWITCH_3IN4OUT_POWER_CRYPTO_BENCH_H
