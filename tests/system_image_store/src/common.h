/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the system-image-store suites: the corpus, the slot platform with
 * fault injection, the metadata volume, SHA-256.
 */

#ifndef SYSTEM_IMAGE_STORE_TEST_COMMON_H_
#define SYSTEM_IMAGE_STORE_TEST_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <system_image_store/system_image_store.h>

#define TEST_MNT "/sys"
#define TEST_DIR "/sys/firmware"

#define SLOT_SIZE    (4U * 1024U * 1024U)
#define SECTOR       4096U
#define TRAILER      65536U
#define MAX_IMAGE    (SLOT_SIZE - TRAILER)
#define TEST_HDR     0x400U
#define RAM_START    0x20000000U
#define RAM_END      0x200C0000U
#define RAM2_START   0x70000000U
#define RAM2_END     0x70800000U
#define EXEC_START   0x02000000U
#define EXEC_END     0x02400000U

/* One file of tests/fixtures/stm32 and what the device must say about it. */
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
extern const struct fixture esp32_merged;

const struct fixture *fixture(const char *name);

/* The slot as the store sees it, and the knobs the tests turn. */
struct test_slot {
	/* The next N-th call of each kind fails (1 = the next one); 0 = never. */
	int fail_read_in;
	int fail_write_in;
	int fail_erase_in;
	/* The N-th write programs the bytes and still reports -EIO. */
	int write_fail_after_in;
	/* The N-th read from now returns one flipped bit (1 = the next one). */
	int corrupt_read_in;
	bool locked;
	int erase_calls;
	int write_calls;
	/* The last erases, [offset, offset + len). */
	uint32_t erase_off[256];
	uint32_t erase_len[256];
};

extern struct test_slot slot;
extern struct sys_img_platform platform;

/* Reset the knobs and counters; the slot's contents stay. */
void slot_reset_knobs(void);
/* Erase the whole slot. */
void slot_erase_all(void);
/* Point the platform at the sim flash device for the stream_flash path, or not. */
void slot_stream(bool on);
/* Raw access that bypasses the knobs. */
void slot_raw_write(uint32_t offset, const uint8_t *data, size_t len);
void slot_raw_read(uint32_t offset, uint8_t *data, size_t len);
void slot_raw_fill(uint32_t offset, uint8_t value, size_t len);

/* Erase the metadata partition and mount an empty LittleFS volume on it. */
void volume_fresh(void);
/* Unmount and mount again, keeping the contents: a reboot of the store. */
void volume_remount(void);

void sha256_of(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
