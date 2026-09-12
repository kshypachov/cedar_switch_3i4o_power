/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_storage.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "fake_storage.h"

static int fake_read(void *ctx, enum device_config_slot slot, void *buf, size_t cap)
{
	struct fake_storage *fs = ctx;

	if (slot >= DEVICE_CONFIG_SLOT_COUNT) {
		return -EINVAL;
	}
	if (!fs->slots[slot].present) {
		return -ENOENT;
	}

	size_t n = MIN(fs->slots[slot].len, cap);

	memcpy(buf, fs->slots[slot].data, n);

	return (int)n;
}

static int fake_write(void *ctx, enum device_config_slot slot, const void *buf, size_t len)
{
	struct fake_storage *fs = ctx;

	if (slot >= DEVICE_CONFIG_SLOT_COUNT) {
		return -EINVAL;
	}
	if (len > FAKE_SLOT_CAPACITY) {
		return -ENOSPC;
	}

	fs->writes[slot]++;

	if (fs->fail_write_slot == (int)slot) {
		fs->fail_write_slot = -1;
		return fs->fail_write_errno;
	}

	if (fs->tear_write_slot == (int)slot) {
		/*
		 * A real torn write leaves a prefix of the new bytes over the
		 * old contents, not an empty slot, so the slot stays present
		 * and keeps its declared length. That is precisely the case
		 * the header CRC has to catch.
		 */
		size_t kept = MIN(fs->tear_after, len);

		fs->tear_write_slot = -1;
		memcpy(fs->slots[slot].data, buf, kept);
		fs->slots[slot].len = len;
		fs->slots[slot].present = true;
		return 0;
	}

	memcpy(fs->slots[slot].data, buf, len);
	fs->slots[slot].len = len;
	fs->slots[slot].present = true;

	return 0;
}

static int fake_erase(void *ctx, enum device_config_slot slot)
{
	struct fake_storage *fs = ctx;

	if (slot >= DEVICE_CONFIG_SLOT_COUNT) {
		return -EINVAL;
	}

	fs->erases[slot]++;

	if (fs->fail_erase_slot == (int)slot) {
		fs->fail_erase_slot = -1;
		return -EIO;
	}

	memset(&fs->slots[slot], 0, sizeof(fs->slots[slot]));

	return 0;
}

void fake_storage_init(struct fake_storage *fs)
{
	memset(fs, 0, sizeof(*fs));
	fake_storage_reset_injection(fs);
}

void fake_storage_reset_injection(struct fake_storage *fs)
{
	fs->fail_write_slot = -1;
	fs->fail_write_errno = -EIO;
	fs->tear_write_slot = -1;
	fs->tear_after = 0;
	fs->fail_erase_slot = -1;
	memset(fs->writes, 0, sizeof(fs->writes));
	memset(fs->erases, 0, sizeof(fs->erases));
}

void fake_storage_bind(struct fake_storage *fs, struct device_config_backend *backend)
{
	backend->read = fake_read;
	backend->write = fake_write;
	backend->erase = fake_erase;
	backend->ctx = fs;
}

void fake_storage_corrupt_byte(struct fake_storage *fs, enum device_config_slot slot,
			       size_t offset)
{
	if (offset < fs->slots[slot].len) {
		fs->slots[slot].data[offset] ^= 0xFFU;
	}
}
