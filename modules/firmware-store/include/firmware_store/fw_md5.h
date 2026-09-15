/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MD5 (RFC 1321), for one purpose: the MD5 record an ESP-IDF partition table
 * carries over its entries. The firmware's PSA configuration has SHA-256 but not
 * MD5 (reports/p6/notes-design-inputs.md), and pulling a legacy hash into the
 * crypto build for 3 KiB of table is more than this small implementation costs.
 * It proves integrity of a table the image builder wrote, not authenticity.
 */

#ifndef FIRMWARE_STORE_FW_MD5_H_
#define FIRMWARE_STORE_FW_MD5_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fw_md5 {
	uint32_t state[4];
	uint64_t length;
	uint8_t block[64];
	uint8_t used;
};

void fw_md5_init(struct fw_md5 *ctx);
void fw_md5_update(struct fw_md5 *ctx, const uint8_t *data, size_t len);
/** Writes the 16-byte digest; @p ctx must be initialised again before reuse. */
void fw_md5_final(struct fw_md5 *ctx, uint8_t digest[16]);

/** One call: the digest of @p len bytes. */
void fw_md5(const uint8_t *data, size_t len, uint8_t digest[16]);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_STORE_FW_MD5_H_ */
