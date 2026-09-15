/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the firmware-store suites: the synthetic corpus and the test volume.
 */

#ifndef FIRMWARE_STORE_TEST_COMMON_H_
#define FIRMWARE_STORE_TEST_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#define TEST_MNT "/fw"
#define TEST_DIR "/fw/firmware"

/* One file of tests/fixtures/esp32/firmware and what the device must say about it. */
struct fixture {
	const char *name;
	const uint8_t *data;
	size_t len;
	/* NULL when the file is valid. */
	const char *code;
	/* A fragment of the expected message; NULL when valid. */
	const char *message;
};

extern const struct fixture corpus[];
extern const size_t corpus_count;

const struct fixture *fixture(const char *name);

/* Erase the test partition and mount an empty LittleFS volume on it. */
void volume_fresh(void);
/* Unmount and mount again, keeping the contents: a reboot of the store. */
void volume_remount(void);

void sha256_of(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
