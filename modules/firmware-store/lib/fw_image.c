/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The full-flash image check. Layouts of the ESP-IDF structures:
 * bootloader_support/include/esp_app_format.h (esp_image_header_t,
 * esp_image_segment_header_t), esp_app_format/include/esp_app_desc.h,
 * esp_bootloader_format/include/esp_bootloader_desc.h and
 * bootloader_support/include/esp_flash_partitions.h of ESP-IDF 5.5.5, which the
 * CP firmware is built with (reports/p6/notes-design-inputs.md).
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <firmware_store/fw_image.h>
#include <firmware_store/fw_md5.h>

/* esp_image_header_t, esp_image_segment_header_t. */
#define IMAGE_MAGIC        0xE9U
#define IMAGE_HEADER_LEN   24U
#define SEGMENT_HEADER_LEN 8U
#define MAX_SEGMENTS       16U
#define CHECKSUM_SEED      0xEFU
#define DIGEST_LEN         32U

/* The description is the first bytes of segment 0: header (24) + segment header (8). */
#define BOOT_DESC_MAGIC    80U
#define BOOT_DESC_LEN      80U
#define APP_DESC_MAGIC     0xABCD5432U
/* Up to and including app_elf_sha256. */
#define APP_DESC_LEN       176U
#define APP_DESC_VERSION   16U
#define APP_DESC_PROJECT   48U
#define APP_DESC_IDF       112U

/* esp_partition_info_t. */
#define PART_ENTRY_LEN     32U
#define PART_MAGIC         0x50AAU
#define PART_MAGIC_MD5     0xEBEBU
#define PART_LABEL_LEN     16U
#define PART_MAX_ENTRIES   (FW_IMAGE_TABLE_MAX_LEN / PART_ENTRY_LEN)

enum walk_phase {
	W_HEADER = 0,
	W_SEG_HDR,
	W_SEG_DATA,
	W_PAD,
	W_CHECKSUM,
	W_DIGEST,
	W_DONE,
};

struct layout_entry {
	const char *label;
	uint8_t type;
	uint8_t subtype;
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
};

struct layout {
	const char *id;
	const char *host_protocol;
	const struct layout_entry *entries;
	size_t count;
};

/*
 * partitions_eh_cp_ota_4m.csv of the CP project, as read from board A's chip in
 * P0 and from the build's partition-table.bin in P6. ota_0 and ota_1 are
 * 0x1c0000 bytes (1792 KiB) each.
 */
static const struct layout_entry cedar_c6_ota_4m[] = {
	{"nvs", 0x01, 0x02, 0x9000, 0x4000, 0},
	{"otadata", 0x01, 0x00, 0xd000, 0x2000, 0},
	{"phy_init", 0x01, 0x01, 0xf000, 0x1000, 0},
	{"ota_0", 0x00, 0x10, 0x10000, 0x1c0000, 0},
	{"ota_1", 0x00, 0x11, 0x1d0000, 0x1c0000, 0},
};

static const struct layout layouts[] = {
	{"cedar-c6-ota-4m-2x1792k", "esp-hosted-mcu-3", cedar_c6_ota_4m, ARRAY_SIZE(cedar_c6_ota_4m)},
};

static void fail(struct fw_image_check *c, enum fw_image_result result, const char *message)
{
	if (c->result == FW_IMAGE_OK) {
		c->result = result;
		c->message = message;
	}
}

static bool all_erased(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xFFU) {
			return false;
		}
	}
	return true;
}

static bool is_boot(const struct fw_image_walk *w)
{
	return w->base == 0U;
}

static void walk_start(struct fw_image_check *c, struct fw_image_walk *w, uint32_t base,
		       uint32_t limit, uint8_t desc_len)
{
	w->base = base;
	w->limit = limit;
	w->phase = W_HEADER;
	w->span_start = base;
	w->span_end = base + IMAGE_HEADER_LEN;
	w->checksum = CHECKSUM_SEED;
	w->desc_len = desc_len;
	w->sha = psa_hash_operation_init();
	if (psa_hash_setup(&w->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		fail(c, FW_IMAGE_INVALID, "SHA-256 is not available on this device");
	}
}

static void set_span(struct fw_image_walk *w, enum walk_phase phase, uint32_t len)
{
	w->phase = (uint8_t)phase;
	w->span_start = w->span_end;
	w->span_end = w->span_start + len;
	w->cap_have = 0;
}

/* Text of an ESP description: NUL-terminated within the field and printable. */
static bool copy_text(char *dst, const uint8_t *src, bool allow_empty)
{
	size_t n = 0;

	while (n < FW_IMAGE_TEXT_MAX && src[n] != 0U) {
		if (src[n] < 0x20U || src[n] > 0x7EU) {
			return false;
		}
		n++;
	}
	if (n == FW_IMAGE_TEXT_MAX || (n == 0U && !allow_empty)) {
		return false;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
	return true;
}

static void check_description(struct fw_image_check *c, struct fw_image_walk *w)
{
	if (is_boot(w)) {
		if (w->desc[0] == BOOT_DESC_MAGIC) {
			return;
		}
		if (sys_get_le32(w->desc) == APP_DESC_MAGIC) {
			fail(c, FW_IMAGE_INVALID,
			     "This is an application image, not the full-flash file (idf.py merge-bin)");
		} else {
			fail(c, FW_IMAGE_INVALID, "The bootloader at 0x0 has no bootloader description");
		}
		return;
	}

	if (sys_get_le32(w->desc) != APP_DESC_MAGIC) {
		fail(c, FW_IMAGE_INVALID, "The application at 0x10000 has no application description");
		return;
	}
	if (!copy_text(c->info.version, &w->desc[APP_DESC_VERSION], false) ||
	    !copy_text(c->info.project_name, &w->desc[APP_DESC_PROJECT], false) ||
	    !copy_text(c->info.idf_version, &w->desc[APP_DESC_IDF], true)) {
		fail(c, FW_IMAGE_INVALID, "The application description does not hold text");
	}
}

/* The span of the current phase is complete: act on it and start the next. */
static void walk_next(struct fw_image_check *c, struct fw_image_walk *w)
{
	do {
		switch (w->phase) {
		case W_HEADER: {
			const uint8_t *h = w->cap;

			if (h[0] != IMAGE_MAGIC) {
				fail(c, FW_IMAGE_INVALID,
				     is_boot(w) ? "No ESP image header at 0x0"
						: "No application image header at 0x10000");
				return;
			}
			if (sys_get_le16(&h[12]) != FW_IMAGE_CHIP_ESP32C6) {
				fail(c, FW_IMAGE_UNSUPPORTED_TARGET,
				     is_boot(w) ? "The bootloader is built for another chip"
						: "The application is built for another chip");
				return;
			}
			w->seg_count = h[1];
			if (w->seg_count == 0U || w->seg_count > MAX_SEGMENTS) {
				fail(c, FW_IMAGE_INVALID, "An image has an impossible segment count");
				return;
			}
			if (h[23] != 1U) {
				fail(c, FW_IMAGE_INVALID, "An image carries no appended SHA-256");
				return;
			}
			w->seg_index = 0;
			set_span(w, W_SEG_HDR, SEGMENT_HEADER_LEN);
			break;
		}
		case W_SEG_HDR: {
			uint32_t len = sys_get_le32(&w->cap[4]);

			/* A bootloader-sized region cannot hold an application's first
			 * segment; let the description say what the file is first. */
			bool may_overrun = is_boot(w) && w->seg_index == 0U;

			if (len > FW_IMAGE_MAX_BYTES ||
			    (!may_overrun && len > w->limit - w->span_end)) {
				fail(c, FW_IMAGE_INVALID,
				     is_boot(w) ? "A bootloader segment runs past 0x8000"
						: "An application segment runs past the end of the file");
				return;
			}
			if (w->seg_index == 0U && len < w->desc_len) {
				fail(c, FW_IMAGE_INVALID,
				     "The first segment is too short to hold the image description");
				return;
			}
			set_span(w, W_SEG_DATA, len);
			break;
		}
		case W_SEG_DATA:
			w->seg_index++;
			if (w->seg_index < w->seg_count) {
				set_span(w, W_SEG_HDR, SEGMENT_HEADER_LEN);
			} else {
				uint32_t rel = w->span_end - w->base;

				w->checksum_at = w->base + (rel / 16U + 1U) * 16U - 1U;
				set_span(w, W_PAD, w->checksum_at - w->span_end);
			}
			break;
		case W_PAD:
			set_span(w, W_CHECKSUM, 1);
			break;
		case W_CHECKSUM:
			set_span(w, W_DIGEST, DIGEST_LEN);
			break;
		case W_DIGEST: {
			uint8_t digest[DIGEST_LEN];
			size_t out = 0;

			if (psa_hash_finish(&w->sha, digest, sizeof(digest), &out) != PSA_SUCCESS ||
			    memcmp(digest, w->cap, DIGEST_LEN) != 0) {
				fail(c, FW_IMAGE_INVALID,
				     is_boot(w) ? "The bootloader's SHA-256 does not match its contents"
						: "The application's SHA-256 does not match its contents");
				return;
			}
			w->end = w->span_end;
			w->phase = W_DONE;
			return;
		}
		default:
			return;
		}
	} while (c->result == FW_IMAGE_OK && w->span_start == w->span_end);
}

/* Bytes [pos, pos + len) of @p w's image; returns how many it consumed. */
static size_t walk_feed(struct fw_image_check *c, struct fw_image_walk *w, uint32_t pos,
			const uint8_t *data, size_t len)
{
	size_t used = 0;

	while (used < len && w->phase != W_DONE && c->result == FW_IMAGE_OK) {
		const uint32_t at = pos + (uint32_t)used;
		const size_t n = MIN(len - used, (size_t)(w->span_end - at));
		const uint8_t *p = &data[used];

		if (w->phase != W_DIGEST) {
			(void)psa_hash_update(&w->sha, p, n);
		}

		switch (w->phase) {
		case W_HEADER:
		case W_SEG_HDR:
		case W_DIGEST:
			memcpy(&w->cap[w->cap_have], p, n);
			w->cap_have += (uint8_t)n;
			break;
		case W_SEG_DATA:
			for (size_t i = 0; i < n; i++) {
				w->checksum ^= p[i];
			}
			if (w->seg_index == 0U && w->desc_have < w->desc_len) {
				const uint32_t into = at - w->span_start;

				if (into < w->desc_len) {
					const size_t k = MIN(n, (size_t)(w->desc_len - into));

					memcpy(&w->desc[into], p, k);
					w->desc_have = (uint8_t)(into + k);
					if (w->desc_have == w->desc_len) {
						check_description(c, w);
					}
				}
			}
			break;
		case W_CHECKSUM:
			if (p[0] != w->checksum) {
				fail(c, FW_IMAGE_INVALID,
				     is_boot(w) ? "The bootloader's checksum does not match"
						: "The application's checksum does not match");
			}
			break;
		default:
			break;
		}

		used += n;
		if (c->result == FW_IMAGE_OK && pos + used == w->span_end) {
			walk_next(c, w);
		}
	}

	return used;
}

static bool label_is(const uint8_t *field, const char *label)
{
	const size_t n = strlen(label);

	if (memcmp(field, label, n) != 0) {
		return false;
	}
	for (size_t i = n; i < PART_LABEL_LEN; i++) {
		if (field[i] != 0U) {
			return false;
		}
	}
	return true;
}

static const struct layout *match_layout(const uint8_t *table, size_t count)
{
	for (size_t l = 0; l < ARRAY_SIZE(layouts); l++) {
		const struct layout *layout = &layouts[l];
		bool same = layout->count == count;

		for (size_t i = 0; same && i < count; i++) {
			const uint8_t *e = &table[i * PART_ENTRY_LEN];
			const struct layout_entry *want = &layout->entries[i];

			same = e[2] == want->type && e[3] == want->subtype &&
			       sys_get_le32(&e[4]) == want->offset &&
			       sys_get_le32(&e[8]) == want->size && label_is(&e[12], want->label) &&
			       sys_get_le32(&e[28]) == want->flags;
		}
		if (same) {
			return layout;
		}
	}
	return NULL;
}

static void check_table(struct fw_image_check *c)
{
	const uint8_t *t = c->table;
	size_t entries = 0;
	bool md5_seen = false;

	for (size_t i = 0; i < FW_IMAGE_TABLE_MAX_LEN; i += PART_ENTRY_LEN) {
		const uint8_t *e = &t[i];
		const uint16_t magic = sys_get_le16(e);

		if (magic == PART_MAGIC && !md5_seen) {
			entries++;
			continue;
		}
		if (magic == PART_MAGIC_MD5 && !md5_seen && entries > 0U) {
			uint8_t digest[16];

			fw_md5(t, i, digest);
			if (memcmp(digest, &e[16], sizeof(digest)) != 0) {
				fail(c, FW_IMAGE_INVALID, "The partition table's MD5 record does not match");
				return;
			}
			md5_seen = true;
			continue;
		}
		if (all_erased(e, PART_ENTRY_LEN) && entries > 0U) {
			if (!md5_seen) {
				fail(c, FW_IMAGE_INVALID, "The partition table has no MD5 record");
				return;
			}
			if (!all_erased(e, FW_IMAGE_TABLE_MAX_LEN - i)) {
				fail(c, FW_IMAGE_INVALID,
				     "Bytes after the partition table's end are not erased");
				return;
			}
			const struct layout *layout = match_layout(t, entries);

			if (layout == NULL) {
				c->layout_mismatch = true;
			} else {
				c->info.layout_id = layout->id;
				c->info.host_protocol = layout->host_protocol;
			}
			return;
		}
		fail(c, FW_IMAGE_INVALID,
		     i == 0U ? "No partition table at 0x8000" : "The partition table has a corrupt entry");
		return;
	}
	fail(c, FW_IMAGE_INVALID, "The partition table has no end");
}

void fw_image_check_begin(struct fw_image_check *c, uint32_t file_size)
{
	memset(c, 0, sizeof(*c));
	c->file_size = file_size;
	c->file_sha = psa_hash_operation_init();
	c->boot.sha = psa_hash_operation_init();
	c->app.sha = psa_hash_operation_init();

	if (psa_crypto_init() != PSA_SUCCESS ||
	    psa_hash_setup(&c->file_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		fail(c, FW_IMAGE_INVALID, "SHA-256 is not available on this device");
		return;
	}
	c->hashing = true;

	walk_start(c, &c->boot, 0, FW_IMAGE_TABLE_OFFSET, BOOT_DESC_LEN);
	walk_start(c, &c->app, FW_IMAGE_APP_OFFSET, file_size, APP_DESC_LEN);

	if (file_size > FW_IMAGE_MAX_BYTES) {
		fail(c, FW_IMAGE_INVALID, "The file reaches ota_1 at 0x1d0000");
	}
}

void fw_image_check_feed(struct fw_image_check *c, const uint8_t *data, size_t len)
{
	if (c->pos >= c->file_size) {
		return;
	}
	len = MIN(len, (size_t)(c->file_size - c->pos));
	if (c->hashing) {
		(void)psa_hash_update(&c->file_sha, data, len);
	}

	size_t off = 0;

	while (off < len && c->result == FW_IMAGE_OK) {
		const uint32_t at = c->pos + (uint32_t)off;
		const uint8_t *p = &data[off];
		size_t n;

		if (at < FW_IMAGE_TABLE_OFFSET) {
			n = MIN(len - off, (size_t)(FW_IMAGE_TABLE_OFFSET - at));
			const size_t used = walk_feed(c, &c->boot, at, p, n);

			if (c->result != FW_IMAGE_OK) {
				break;
			}
			if (used < n && !all_erased(&p[used], n - used)) {
				fail(c, FW_IMAGE_INVALID,
				     "Bytes between the bootloader and the partition table are not erased");
			} else if (at + n == FW_IMAGE_TABLE_OFFSET && c->boot.phase != W_DONE) {
				fail(c, FW_IMAGE_INVALID, "The bootloader runs past 0x8000");
			}
		} else if (at < FW_IMAGE_TABLE_OFFSET + FW_IMAGE_TABLE_MAX_LEN) {
			n = MIN(len - off, (size_t)(FW_IMAGE_TABLE_OFFSET + FW_IMAGE_TABLE_MAX_LEN - at));
			memcpy(&c->table[at - FW_IMAGE_TABLE_OFFSET], p, n);
			if (at + n == FW_IMAGE_TABLE_OFFSET + FW_IMAGE_TABLE_MAX_LEN) {
				check_table(c);
			}
		} else if (at < FW_IMAGE_DATA_OFFSET) {
			n = MIN(len - off, (size_t)(FW_IMAGE_DATA_OFFSET - at));
			if (!all_erased(p, n)) {
				fail(c, FW_IMAGE_INVALID, "Bytes after the partition table are not erased");
			}
		} else if (at < FW_IMAGE_APP_OFFSET) {
			n = MIN(len - off, (size_t)(FW_IMAGE_APP_OFFSET - at));
			if (!all_erased(p, n)) {
				fail(c, FW_IMAGE_INVALID,
				     "NVS, otadata and phy_init (0x9000-0x10000) are not erased");
			}
		} else {
			n = len - off;
			const size_t used = walk_feed(c, &c->app, at, p, n);

			if (c->result == FW_IMAGE_OK && used < n) {
				fail(c, FW_IMAGE_INVALID, "Bytes follow the end of the application");
			}
		}
		off += n;
	}

	c->pos += (uint32_t)len;
}

bool fw_image_check_failed(const struct fw_image_check *c)
{
	return c->result != FW_IMAGE_OK;
}

void fw_image_check_abort(struct fw_image_check *c)
{
	(void)psa_hash_abort(&c->file_sha);
	(void)psa_hash_abort(&c->boot.sha);
	(void)psa_hash_abort(&c->app.sha);
	c->hashing = false;
}

enum fw_image_result fw_image_check_end(struct fw_image_check *c, struct fw_image_info *info,
					const char **message)
{
	if (c->pos < c->file_size) {
		fail(c, FW_IMAGE_INVALID, "The file is shorter than its declared size");
	} else if (c->file_size <= FW_IMAGE_APP_OFFSET) {
		fail(c, FW_IMAGE_INVALID, "The file ends before the application at 0x10000");
	} else if (c->app.phase != W_DONE) {
		fail(c, FW_IMAGE_INVALID, "The application is cut short");
	}

	if (c->hashing && c->pos == c->file_size) {
		size_t out = 0;

		if (psa_hash_finish(&c->file_sha, c->info.sha256, sizeof(c->info.sha256), &out) !=
		    PSA_SUCCESS) {
			memset(c->info.sha256, 0, sizeof(c->info.sha256));
		}
	}
	fw_image_check_abort(c);

	c->info.bootloader_bytes = c->boot.phase == W_DONE ? c->boot.end - c->boot.base : 0U;
	c->info.app_bytes = c->app.phase == W_DONE ? c->app.end - c->app.base : 0U;

	enum fw_image_result result = c->result;
	const char *why = c->message;

	if (result == FW_IMAGE_OK && c->layout_mismatch) {
		result = FW_IMAGE_INCOMPATIBLE;
		why = "The partition table's layout is not one this device supports";
	}
	if (result != FW_IMAGE_OK && result != FW_IMAGE_INCOMPATIBLE) {
		c->info.layout_id = NULL;
		c->info.host_protocol = NULL;
	}

	if (info != NULL) {
		*info = c->info;
	}
	if (message != NULL) {
		*message = result == FW_IMAGE_OK ? NULL : why;
	}
	return result;
}

const char *fw_image_result_code(enum fw_image_result result)
{
	switch (result) {
	case FW_IMAGE_INVALID:
		return "invalid_image";
	case FW_IMAGE_UNSUPPORTED_TARGET:
		return "unsupported_target";
	case FW_IMAGE_INCOMPATIBLE:
		return "incompatible_firmware";
	default:
		return NULL;
	}
}
