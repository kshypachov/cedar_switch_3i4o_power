/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fw_image: is this file a full-flash image the ESP32-C6 will boot?
 *
 * Contract: "Формат файла" in docs/device-development/api-contract.md (P6), the
 * owner's decision №1 (reports/p6/README.md): the only image format of the first
 * version is `raw_full_flash`, the file `idf.py merge-bin` produces, written to
 * the coprocessor whole from offset 0x0. The checks and the reasons for each:
 *
 * - **A pure, streaming check.** The file is 1.47 MiB on a LittleFS volume and
 *   the device has no room for it in RAM, so the checker takes the file in
 *   pieces, in order (fw_image_check_feed()), and keeps only what it needs: the
 *   partition table (3 KiB), two descriptions, a few headers, and three running
 *   SHA-256 contexts. No file I/O, no allocation, no clock: the sim tier feeds it
 *   buffers, firmware-store feeds it the staged file.
 *
 * - **Four regions, each checked for what the chip will do with it:**
 *   1. `[0x0, 0x8000)` - the second-stage bootloader the ROM loads: an ESP image
 *      header (0xE9, ESP32-C6 chip id, 1..16 segments, an appended SHA-256),
 *      segments inside the region, the XOR checksum, the digest, and the
 *      bootloader description (magic byte 80) at 0x20. The rest of the region
 *      must be erased (0xFF): anything else would be written over nothing the
 *      bootloader reads, and is most likely a different file.
 *   2. `[0x8000, 0x9000)` - the partition table the bootloader reads: 32-byte
 *      entries, the MD5 record over them (fw_md5.h), the end marker, erased
 *      bytes after it, and a layout that is **exactly** one this device
 *      supports. The layout decides where ota_0 is and how large the app may
 *      be, so a different one is `incompatible`, not merely unusual.
 *   3. `[0x9000, 0x10000)` - NVS, otadata and phy_init, all erased: a blank
 *      otadata boots ota_0, and an erased NVS is what the owner accepted
 *      (Wi-Fi secrets live on the STM32).
 *   4. `[0x10000, end)` - the application: header, chip id, segments, checksum,
 *      appended SHA-256 and the application description (magic 0xABCD5432) at
 *      0x10020. The application ends exactly where the file ends, and the file
 *      never reaches ota_1 (FW_IMAGE_MAX_BYTES).
 *
 * - **Three outcomes the contract names.** A foreign chip id in the bootloader or
 *   the application is `unsupported_target`; a well-formed table with another
 *   layout is `incompatible_firmware`; everything else - a bare application
 *   `.bin`, a truncated file, a bad digest, junk between the parts - is
 *   `invalid_image`, with a message saying which. The first structural fault
 *   stops the checks (later ones would be noise); a layout mismatch does not, so
 *   a file that is also broken reports `invalid_image`.
 *
 * - **What it does not decide.** Which CP build this is (Wi-Fi or OpenThread,
 *   the owner's deferred decision №4): the two are indistinguishable by their
 *   headers, so `host_protocol` is the profile's value and is not checked
 *   against the image. Nothing here is a signature: the digests find damage,
 *   not authors.
 *
 * Threading: a struct fw_image_check belongs to one thread for its whole life.
 */

#ifndef FIRMWARE_STORE_FW_IMAGE_H_
#define FIRMWARE_STORE_FW_IMAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the partition table starts, and the most bytes it may take. */
#define FW_IMAGE_TABLE_OFFSET  0x8000U
#define FW_IMAGE_TABLE_MAX_LEN 0xC00U
/** First byte after the table's region: NVS starts here. */
#define FW_IMAGE_DATA_OFFSET   0x9000U
/** Where ota_0, and therefore the application, starts. */
#define FW_IMAGE_APP_OFFSET    0x10000U
/** The file may not reach ota_1: the application slot is 0x1c0000 bytes. */
#define FW_IMAGE_MAX_BYTES     0x1d0000U
/** esp_chip_id_t of the ESP32-C6. */
#define FW_IMAGE_CHIP_ESP32C6  13U

/** The longest text field of an ESP description (version, project, IDF). */
#define FW_IMAGE_TEXT_MAX 32

enum fw_image_result {
	FW_IMAGE_OK = 0,
	/** `invalid_image`: not a full-flash image this chip boots. */
	FW_IMAGE_INVALID,
	/** `unsupported_target`: built for another chip. */
	FW_IMAGE_UNSUPPORTED_TARGET,
	/** `incompatible_firmware`: a partition layout this device does not use. */
	FW_IMAGE_INCOMPATIBLE,
};

/** What a checked file says about itself. */
struct fw_image_info {
	/** esp_app_desc_t.version, project_name and idf_ver, NUL-terminated. */
	char version[FW_IMAGE_TEXT_MAX + 1];
	char project_name[FW_IMAGE_TEXT_MAX + 1];
	char idf_version[FW_IMAGE_TEXT_MAX + 1];
	/** The supported layout the table matched, or NULL. Static strings. */
	const char *layout_id;
	/** The profile's host protocol; not checked against the image. */
	const char *host_protocol;
	/** Bytes of the bootloader image and of the application image. */
	uint32_t bootloader_bytes;
	uint32_t app_bytes;
	/** SHA-256 of the whole file, valid once all of it was fed. */
	uint8_t sha256[32];
};

/** Walks one ESP image (bootloader or application). Private to fw_image.c. */
struct fw_image_walk {
	uint32_t base;
	uint32_t limit;
	uint8_t phase;
	uint8_t seg_count;
	uint8_t seg_index;
	uint8_t checksum;
	/* The span of the current phase, absolute offsets. */
	uint32_t span_start;
	uint32_t span_end;
	uint32_t checksum_at;
	uint32_t end;
	/* A header, segment header or digest being collected. */
	uint8_t cap[32];
	uint8_t cap_have;
	/* The description at base + 0x20. */
	uint8_t desc[176];
	uint8_t desc_len;
	uint8_t desc_have;
	psa_hash_operation_t sha;
};

/** One check in progress. Large (4 KiB): keep it static, not on a stack. */
struct fw_image_check {
	uint32_t file_size;
	uint32_t pos;
	enum fw_image_result result;
	const char *message;
	bool layout_mismatch;
	bool hashing;
	psa_hash_operation_t file_sha;
	struct fw_image_walk boot;
	struct fw_image_walk app;
	uint8_t table[FW_IMAGE_TABLE_MAX_LEN];
	struct fw_image_info info;
};

/**
 * @brief Start checking a file of @p file_size bytes.
 *
 * Calls psa_crypto_init(). A size that cannot be a full-flash image (not past
 * the application offset, or beyond FW_IMAGE_MAX_BYTES) fails the check at once;
 * the whole-file digest is still computed.
 */
void fw_image_check_begin(struct fw_image_check *c, uint32_t file_size);

/**
 * @brief The next @p len bytes of the file, in order.
 *
 * Bytes past the declared size are ignored. After the first structural fault
 * only the whole-file digest keeps running, so a caller may stop feeding early
 * when fw_image_check_failed() is true and it does not need the digest.
 */
void fw_image_check_feed(struct fw_image_check *c, const uint8_t *data, size_t len);

/** @brief A structural fault was found (the result can no longer be OK). */
bool fw_image_check_failed(const struct fw_image_check *c);

/**
 * @brief Finish: the outcome, what the file says about itself, and why not.
 *
 * A file fed short of its declared size is `invalid_image`. @p info is filled as
 * far as the check got (the digest only when every byte was fed); @p message is
 * a static English sentence, NULL on FW_IMAGE_OK. Releases the hash contexts.
 */
enum fw_image_result fw_image_check_end(struct fw_image_check *c, struct fw_image_info *info,
					const char **message);

/** @brief Release the hash contexts of a check that will not be finished. */
void fw_image_check_abort(struct fw_image_check *c);

/** @brief The contract's code: "invalid_image", "unsupported_target", ...; NULL for OK. */
const char *fw_image_result_code(enum fw_image_result result);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_STORE_FW_IMAGE_H_ */
