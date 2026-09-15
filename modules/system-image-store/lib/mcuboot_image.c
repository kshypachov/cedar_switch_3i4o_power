/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The streaming MCUboot image check. See include/system_image_store/mcuboot_image.h.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <system_image_store/mcuboot_image.h>

/* Header fields, little endian (bootutil/image.h, struct image_header). */
#define HDR_MAGIC     0
#define HDR_HDR_SIZE  8
#define HDR_PROT_SIZE 10
#define HDR_IMG_SIZE  12
#define HDR_FLAGS     16
#define HDR_MAJOR     20
#define HDR_MINOR     21
#define HDR_REVISION  22
#define HDR_BUILD     24

#define TLV_HEADER 4U
#define VECTORS    8U

static void fail(struct mcuboot_image_check *c, enum mcuboot_image_result result,
		 const char *message)
{
	if (c->result == MCUBOOT_IMAGE_OK) {
		c->result = result;
		c->message = message;
	}
}

void mcuboot_image_check_begin(struct mcuboot_image_check *c,
			       const struct mcuboot_image_params *params, uint32_t file_size)
{
	memset(c, 0, sizeof(*c));
	c->params = params;
	c->file_size = file_size;
	c->file_sha = psa_hash_operation_init();
	c->image_sha = psa_hash_operation_init();

	if (psa_crypto_init() != PSA_SUCCESS ||
	    psa_hash_setup(&c->file_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The device could not compute SHA-256");
		return;
	}
	if (psa_hash_setup(&c->image_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		(void)psa_hash_abort(&c->file_sha);
		fail(c, MCUBOOT_IMAGE_INVALID, "The device could not compute SHA-256");
		return;
	}
	c->hashing = true;

	if (params == NULL || file_size < MCUBOOT_IMAGE_HEADER_SIZE + VECTORS + TLV_HEADER) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The file is too small to be an MCUboot image");
	}
}

/* The header is complete: everything it alone can tell. */
static void check_header(struct mcuboot_image_check *c)
{
	const uint8_t *h = c->header;
	const uint16_t hdr_size = sys_get_le16(&h[HDR_HDR_SIZE]);
	const uint16_t prot_size = sys_get_le16(&h[HDR_PROT_SIZE]);
	const uint32_t img_size = sys_get_le32(&h[HDR_IMG_SIZE]);
	const uint32_t flags = sys_get_le32(&h[HDR_FLAGS]);
	const uint64_t tlv_start = (uint64_t)hdr_size + img_size;
	const uint64_t hash_end = tlv_start + prot_size;

	if (sys_get_le32(&h[HDR_MAGIC]) != MCUBOOT_IMAGE_MAGIC) {
		fail(c, MCUBOOT_IMAGE_INVALID,
		     "This is not an MCUboot image: the header magic is missing");
		return;
	}
	if (hdr_size != c->params->header_size) {
		fail(c, MCUBOOT_IMAGE_UNSUPPORTED_TARGET,
		     "The image's header size is not the one this board's firmware is built with");
		return;
	}
	if ((flags & MCUBOOT_F_REFUSED) != 0U) {
		fail(c, MCUBOOT_IMAGE_INVALID,
		     "The image is encrypted, compressed, loaded into RAM or not bootable; this "
		     "board's MCUboot cannot boot it");
		return;
	}
	if (img_size < VECTORS) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The image has no vector table");
		return;
	}
	if (hash_end + TLV_HEADER > c->file_size) {
		fail(c, MCUBOOT_IMAGE_INVALID,
		     "The image's header declares more bytes than the file has");
		return;
	}
	if ((uint64_t)c->file_size - tlv_start > CONFIG_SYSTEM_IMAGE_STORE_TLV_MAX) {
		fail(c, MCUBOOT_IMAGE_INVALID,
		     "The file continues too far past the image: a padded or merged file, or a "
		     "TLV area larger than this device accepts");
		return;
	}

	c->tlv_start = (uint32_t)tlv_start;
	c->hash_end = (uint32_t)hash_end;
	c->info.major = h[HDR_MAJOR];
	c->info.minor = h[HDR_MINOR];
	c->info.revision = sys_get_le16(&h[HDR_REVISION]);
	c->info.build = sys_get_le32(&h[HDR_BUILD]);
	snprintf(c->info.version, sizeof(c->info.version), "%u.%u.%u+%u", c->info.major,
		 c->info.minor, c->info.revision, c->info.build);
	c->header_ok = true;
}

/* Copy the overlap of [pos, pos + len) with [start, start + cap) into dst. */
static void capture(uint32_t pos, const uint8_t *data, size_t len, uint32_t start, uint32_t cap,
		    uint8_t *dst)
{
	const uint64_t from = MAX((uint64_t)pos, (uint64_t)start);
	const uint64_t to = MIN((uint64_t)pos + len, (uint64_t)start + cap);

	if (from < to) {
		memcpy(&dst[from - start], &data[from - pos], (size_t)(to - from));
	}
}

void mcuboot_image_check_feed(struct mcuboot_image_check *c, const uint8_t *data, size_t len)
{
	if (c->pos >= c->file_size || data == NULL) {
		return;
	}
	len = MIN(len, (size_t)(c->file_size - c->pos));
	if (len == 0U) {
		return;
	}
	if (c->hashing) {
		(void)psa_hash_update(&c->file_sha, data, len);
	}

	/* The header: always inside what MCUboot hashes. */
	if (c->pos < MCUBOOT_IMAGE_HEADER_SIZE) {
		const size_t n = MIN(len, (size_t)(MCUBOOT_IMAGE_HEADER_SIZE - c->pos));

		memcpy(&c->header[c->pos], data, n);
		if (c->hashing) {
			(void)psa_hash_update(&c->image_sha, data, n);
		}
		c->pos += (uint32_t)n;
		data += n;
		len -= n;
		if (c->pos == MCUBOOT_IMAGE_HEADER_SIZE && c->result == MCUBOOT_IMAGE_OK) {
			check_header(c);
		}
	}

	if (len > 0U && c->result == MCUBOOT_IMAGE_OK && c->header_ok) {
		const uint16_t hdr_size = c->params->header_size;

		if (c->hashing && c->pos < c->hash_end) {
			(void)psa_hash_update(&c->image_sha, data,
					      MIN(len, (size_t)(c->hash_end - c->pos)));
		}
		capture(c->pos, data, len, hdr_size, VECTORS, c->vectors);
		capture(c->pos, data, len, c->tlv_start, c->file_size - c->tlv_start, c->tlv);
	}
	c->pos += (uint32_t)len;
}

/* The TLV areas, now whole in c->tlv. */
static void check_tlvs(struct mcuboot_image_check *c)
{
	const uint32_t area = c->file_size - c->tlv_start;
	const uint32_t prot = c->hash_end - c->tlv_start;
	uint32_t off = 0;
	bool found = false;

	if (prot > 0U) {
		if (prot < TLV_HEADER || sys_get_le16(c->tlv) != MCUBOOT_TLV_PROT_INFO_MAGIC ||
		    sys_get_le16(&c->tlv[2]) != prot) {
			fail(c, MCUBOOT_IMAGE_INVALID, "The image's protected TLV area is malformed");
			return;
		}
		off = prot;
	}
	if (sys_get_le16(&c->tlv[off]) != MCUBOOT_TLV_INFO_MAGIC) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The image has no TLV area after its body");
		return;
	}
	const uint32_t end = off + sys_get_le16(&c->tlv[off + 2U]);

	if (end > area) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The image's TLV area runs past the end of the file");
		return;
	}
	if (end < area) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The file has bytes after the image's TLV area");
		return;
	}
	for (uint32_t p = off + TLV_HEADER; p < end;) {
		uint16_t type;
		uint16_t len;

		if (p + TLV_HEADER > end) {
			fail(c, MCUBOOT_IMAGE_INVALID, "The image's TLV area is malformed");
			return;
		}
		type = sys_get_le16(&c->tlv[p]);
		len = sys_get_le16(&c->tlv[p + 2U]);
		if (p + TLV_HEADER + len > end) {
			fail(c, MCUBOOT_IMAGE_INVALID, "The image's TLV area is malformed");
			return;
		}
		if (type == MCUBOOT_TLV_SHA256) {
			if (len != sizeof(c->info.image_hash)) {
				fail(c, MCUBOOT_IMAGE_INVALID,
				     "The image's SHA-256 TLV has the wrong length");
				return;
			}
			memcpy(c->info.image_hash, &c->tlv[p + TLV_HEADER], len);
			found = true;
		}
		p += TLV_HEADER + len;
	}
	if (!found) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The image has no SHA-256 TLV");
	}
}

static bool in_ram(const struct mcuboot_image_params *p, uint32_t sp)
{
	for (size_t i = 0; i < ARRAY_SIZE(p->ram); i++) {
		if (p->ram[i].end != 0U && sp > p->ram[i].start && sp <= p->ram[i].end) {
			return true;
		}
	}
	return false;
}

static void check_vectors(struct mcuboot_image_check *c)
{
	const uint32_t sp = sys_get_le32(c->vectors);
	const uint32_t reset = sys_get_le32(&c->vectors[4]);
	const uint32_t handler = reset & ~1U;

	if (!in_ram(c->params, sp)) {
		fail(c, MCUBOOT_IMAGE_UNSUPPORTED_TARGET,
		     "The image's initial stack pointer is not in this board's RAM");
		return;
	}
	if ((reset & 1U) == 0U || handler < c->params->exec.start ||
	    handler >= c->params->exec.end) {
		fail(c, MCUBOOT_IMAGE_UNSUPPORTED_TARGET,
		     "The image's reset vector is not in this board's application flash");
	}
}

enum mcuboot_image_result mcuboot_image_check_end(struct mcuboot_image_check *c,
						  struct mcuboot_image_info *info,
						  const char **message)
{
	uint8_t image_hash[32];
	size_t n = 0;
	bool image_hashed = false;

	if (c->hashing) {
		if (c->pos == c->file_size) {
			(void)psa_hash_finish(&c->file_sha, c->info.sha256, sizeof(c->info.sha256),
					      &n);
			image_hashed = psa_hash_finish(&c->image_sha, image_hash, sizeof(image_hash),
						       &n) == PSA_SUCCESS;
		} else {
			(void)psa_hash_abort(&c->file_sha);
		}
		if (!image_hashed) {
			(void)psa_hash_abort(&c->image_sha);
		}
		c->hashing = false;
	}

	if (c->result == MCUBOOT_IMAGE_OK && c->pos != c->file_size) {
		fail(c, MCUBOOT_IMAGE_INVALID, "The file ended before its declared size");
	}
	if (c->result == MCUBOOT_IMAGE_OK) {
		check_tlvs(c);
	}
	if (c->result == MCUBOOT_IMAGE_OK &&
	    (!image_hashed || memcmp(image_hash, c->info.image_hash, sizeof(image_hash)) != 0)) {
		fail(c, MCUBOOT_IMAGE_INVALID,
		     "The image's SHA-256 does not match the one in its TLV area");
	}
	if (c->result == MCUBOOT_IMAGE_OK) {
		check_vectors(c);
	}

	if (info != NULL) {
		*info = c->info;
	}
	if (message != NULL) {
		*message = c->message;
	}
	return c->result;
}

void mcuboot_image_check_abort(struct mcuboot_image_check *c)
{
	if (c->hashing) {
		(void)psa_hash_abort(&c->file_sha);
		(void)psa_hash_abort(&c->image_sha);
		c->hashing = false;
	}
}

const char *mcuboot_image_result_code(enum mcuboot_image_result result)
{
	switch (result) {
	case MCUBOOT_IMAGE_INVALID:
		return "invalid_image";
	case MCUBOOT_IMAGE_UNSUPPORTED_TARGET:
		return "unsupported_target";
	default:
		return NULL;
	}
}
