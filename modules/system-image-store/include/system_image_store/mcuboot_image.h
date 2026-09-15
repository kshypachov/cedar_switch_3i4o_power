/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mcuboot_image: is this file an application image MCUboot on this board will
 * swap in and boot?
 *
 * Contract: "Обновление STM32" in docs/device-development/api-contract.md and the
 * design in docs/device-development/reports/stm32-update/README.md. The image is
 * `zephyr.signed.bin` of the STM32 application: imgtool's header, the body, the
 * TLV areas, nothing after them. The checks and why:
 *
 * - **A pure, streaming check**, like firmware-store's fw_image: the file lives in
 *   MCUboot's secondary slot on the SPI NOR, up to 4 MiB, and is fed in pieces,
 *   in order. The check keeps the 32-byte header, the first two vector table
 *   words, the TLV areas (bounded by CONFIG_SYSTEM_IMAGE_STORE_TLV_MAX) and two
 *   running SHA-256 contexts: the whole file, and the range MCUboot hashes.
 *
 * - **What MCUboot itself verifies, verified first** (bootutil_img_validate with
 *   BOOT_SIGNATURE_TYPE_NONE): magic 0x96f3b83d, the TLV info magic after the
 *   body (0x6907, and 0x6908 before it when ih_protect_tlv_size says there is a
 *   protected area), and the SHA-256 TLV (type 0x10, 32 bytes) equal to the
 *   SHA-256 of header + body + protected TLV area. An image that fails any of
 *   these would be refused by MCUboot after the reset; refusing it here keeps
 *   the swap from being requested at all. Failures are `invalid_image`.
 *
 * - **The file is exactly the image.** header + body + protected TLV + TLV must
 *   end where the file ends. Anything after would be written into the slot and
 *   ignored by MCUboot - most likely a different file (a padded image, or a
 *   merged hex turned binary). `invalid_image`.
 *
 * - **Flags this board's MCUboot cannot boot**: encrypted (AES128/AES256), RAM
 *   load, non-bootable, compressed. `invalid_image` - these are not images for a
 *   swap-scratch loader without encryption keys, whatever their target.
 *
 * - **Built for this board**, `unsupported_target`: ih_hdr_size must equal the
 *   application's ROM_START_OFFSET (MCUboot jumps to slot base + header size),
 *   and the vector table at that offset must hold an initial stack pointer in
 *   one of the board's RAM ranges and a Thumb reset vector inside the
 *   application's execution window. An image linked for another board or
 *   another load address fails one of these while its digest is fine.
 *
 * - **Order of verdicts.** Magic, then header size (a file whose first bytes are
 *   not an MCUboot header says nothing about targets), then flags and sizes as
 *   bytes arrive; at the end the TLV structure, the digest, and only then the
 *   vector table: damage explains a wrong vector as well as a wrong board does,
 *   so a broken digest reports `invalid_image`. The first fault stops the rest.
 *
 * - **Not a signature.** Digests find damage, not authors (owner's decision Д3:
 *   no signing for now); signature_verified stays null.
 *
 * Threading: a struct mcuboot_image_check belongs to one thread for its life.
 */

#ifndef SYSTEM_IMAGE_STORE_MCUBOOT_IMAGE_H_
#define SYSTEM_IMAGE_STORE_MCUBOOT_IMAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCUBOOT_IMAGE_MAGIC         0x96f3b83dU
#define MCUBOOT_IMAGE_HEADER_SIZE   32U
#define MCUBOOT_TLV_INFO_MAGIC      0x6907U
#define MCUBOOT_TLV_PROT_INFO_MAGIC 0x6908U
#define MCUBOOT_TLV_SHA256          0x10U

#define MCUBOOT_F_ENCRYPTED_AES128 0x00000004U
#define MCUBOOT_F_ENCRYPTED_AES256 0x00000008U
#define MCUBOOT_F_NON_BOOTABLE     0x00000010U
#define MCUBOOT_F_RAM_LOAD         0x00000020U
#define MCUBOOT_F_COMPRESSED_LZMA1 0x00000200U
#define MCUBOOT_F_COMPRESSED_LZMA2 0x00000400U
#define MCUBOOT_F_COMPRESSED_ARM   0x00000800U

/** The flags refused, as one mask. */
#define MCUBOOT_F_REFUSED                                                                          \
	(MCUBOOT_F_ENCRYPTED_AES128 | MCUBOOT_F_ENCRYPTED_AES256 | MCUBOOT_F_NON_BOOTABLE |        \
	 MCUBOOT_F_RAM_LOAD | MCUBOOT_F_COMPRESSED_LZMA1 | MCUBOOT_F_COMPRESSED_LZMA2 |            \
	 MCUBOOT_F_COMPRESSED_ARM)

/** "255.255.65535+4294967295" and its terminator. */
#define MCUBOOT_IMAGE_VERSION_MAX 24

#define MCUBOOT_IMAGE_RAM_RANGES 4

enum mcuboot_image_result {
	MCUBOOT_IMAGE_OK = 0,
	/** `invalid_image`: not an image MCUboot on this board would boot. */
	MCUBOOT_IMAGE_INVALID,
	/** `unsupported_target`: a well-formed image for another board or address. */
	MCUBOOT_IMAGE_UNSUPPORTED_TARGET,
};

/** A half-open address range [start, end). */
struct mcuboot_image_range {
	uint32_t start;
	uint32_t end;
};

/** What "built for this board" means. Owned by the caller, must outlive the check. */
struct mcuboot_image_params {
	/** ih_hdr_size the application is built with (CONFIG_ROM_START_OFFSET). */
	uint16_t header_size;
	/**
	 * Where the initial stack pointer may point. The stack is full-descending, so
	 * the top of a RAM range is a valid initial SP: sp is accepted when
	 * start < sp <= end. Unused entries have end == 0.
	 */
	struct mcuboot_image_range ram[MCUBOOT_IMAGE_RAM_RANGES];
	/** Where the reset handler may be: start <= (vector & ~1) < end, bit 0 set. */
	struct mcuboot_image_range exec;
};

/** What a checked file says about itself. */
struct mcuboot_image_info {
	/** "major.minor.revision+build". */
	char version[MCUBOOT_IMAGE_VERSION_MAX + 1];
	uint8_t major;
	uint8_t minor;
	uint16_t revision;
	uint32_t build;
	/** The SHA-256 TLV: what MCUboot will compare, and what identifies the image. */
	uint8_t image_hash[32];
	/** SHA-256 of the whole file, valid once all of it was fed. */
	uint8_t sha256[32];
};

/** One check in progress. Keep it static, not on a stack (TLV buffer). */
struct mcuboot_image_check {
	const struct mcuboot_image_params *params;
	uint32_t file_size;
	uint32_t pos;
	enum mcuboot_image_result result;
	const char *message;
	bool hashing;
	bool header_ok;
	uint8_t header[MCUBOOT_IMAGE_HEADER_SIZE];
	uint8_t vectors[8];
	/** End of what MCUboot hashes: header + body + protected TLV area. */
	uint32_t hash_end;
	/** Start of the TLV areas kept: header + body. */
	uint32_t tlv_start;
	uint8_t tlv[CONFIG_SYSTEM_IMAGE_STORE_TLV_MAX];
	psa_hash_operation_t file_sha;
	psa_hash_operation_t image_sha;
	struct mcuboot_image_info info;
};

/**
 * @brief Start checking a file of @p file_size bytes against @p params.
 *
 * Calls psa_crypto_init(). A file too small for a header and a vector table
 * fails at once; the whole-file digest is still computed.
 */
void mcuboot_image_check_begin(struct mcuboot_image_check *c,
			       const struct mcuboot_image_params *params, uint32_t file_size);

/**
 * @brief The next @p len bytes of the file, in order. Bytes past the declared
 *        file size are ignored.
 */
void mcuboot_image_check_feed(struct mcuboot_image_check *c, const uint8_t *data, size_t len);

/**
 * @brief Finish: the verdict, what the image says (valid fields depend on how
 *        far it got - info->sha256 whenever the whole file was fed), and a
 *        sentence for ErrorDetail.message (static; NULL when OK).
 */
enum mcuboot_image_result mcuboot_image_check_end(struct mcuboot_image_check *c,
						  struct mcuboot_image_info *info,
						  const char **message);

/** @brief Stop a check that will not be ended; releases the hash contexts. */
void mcuboot_image_check_abort(struct mcuboot_image_check *c);

/** @brief The contract's code: "invalid_image", "unsupported_target"; NULL for OK. */
const char *mcuboot_image_result_code(enum mcuboot_image_result result);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_IMAGE_STORE_MCUBOOT_IMAGE_H_ */
