/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The staged STM32 image: bytes in MCUboot's slot 2, state in a file. See
 * include/system_image_store/system_image_store.h for the contract and README.md
 * for how it fits the system.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include <system_image_store/system_image_store.h>

LOG_MODULE_REGISTER(system_image_store, CONFIG_SYSTEM_IMAGE_STORE_LOG_LEVEL);

#define ID_PREFIX    "sysimg_"
#define NAME_META    "sysimg.meta"
#define NAME_TMP     "sysimg.meta.tmp"
#define PATH_MAX_LEN 96

#define META_MAGIC   0x4d495343U /* "CSIM" */
#define META_VERSION 1U

/* The metadata file. Packed and zero-filled, so the CRC covers no stray padding. */
struct meta {
	uint32_t magic;
	uint16_t version;
	uint8_t state;
	uint8_t has_image;
	char id[SYS_IMG_ID_LEN + 1];
	char filename[SYS_IMG_FILENAME_MAX + 1];
	uint32_t size;
	uint32_t received;
	uint8_t sha256[32];
	char version_text[MCUBOOT_IMAGE_VERSION_MAX + 1];
	uint8_t major;
	uint8_t minor;
	uint16_t revision;
	uint32_t build;
	uint8_t image_hash[32];
	char error_code[SYS_IMG_ERROR_CODE_MAX + 1];
	char error_message[SYS_IMG_ERROR_MESSAGE_MAX + 1];
	uint32_t crc;
} __packed;

static struct {
	struct k_mutex lock;
	bool ready;
	const struct sys_img_platform *p;
	char meta[PATH_MAX_LEN];
	char tmp[PATH_MAX_LEN];

	bool present;
	struct meta m;

	char job[SYS_IMG_JOB_ID_MAX + 1];
	bool in_use;
	bool pending;
	bool verifying;
	uint32_t pending_offset;
	size_t pending_len;
	int64_t last_activity;
} st;

static bool lock_ready;
static uint8_t staging[CONFIG_SYSTEM_IMAGE_STORE_CHUNK_MAX];
static uint8_t read_buf[CONFIG_SYSTEM_IMAGE_STORE_READ_BUF];
static uint8_t repair_buf[CONFIG_SYSTEM_IMAGE_STORE_SECTOR_MAX];
static struct mcuboot_image_check check;

/* -- metadata --------------------------------------------------------------------- */

static uint32_t meta_crc(const struct meta *m)
{
	return crc32_ieee((const uint8_t *)m, offsetof(struct meta, crc));
}

static bool terminated(const char *s, size_t cap)
{
	return memchr(s, '\0', cap) != NULL;
}

static uint32_t max_size(const struct sys_img_platform *p)
{
	return p->slot_size - p->trailer_bytes;
}

static bool meta_valid(const struct meta *m)
{
	return m->magic == META_MAGIC && m->version == META_VERSION && m->crc == meta_crc(m) &&
	       (m->state == SYS_IMG_RECEIVING || m->state == SYS_IMG_READY ||
		m->state == SYS_IMG_FAILED) &&
	       terminated(m->id, sizeof(m->id)) && strlen(m->id) == SYS_IMG_ID_LEN &&
	       strncmp(m->id, ID_PREFIX, strlen(ID_PREFIX)) == 0 &&
	       terminated(m->filename, sizeof(m->filename)) && m->filename[0] != '\0' &&
	       m->size > 0U && m->size <= max_size(st.p) && m->received <= m->size &&
	       terminated(m->version_text, sizeof(m->version_text)) &&
	       terminated(m->error_code, sizeof(m->error_code)) &&
	       terminated(m->error_message, sizeof(m->error_message));
}

static int exists(const char *path)
{
	struct fs_dirent entry;

	return fs_stat(path, &entry);
}

/* fs_unlink() logs an error for a missing path: look first. */
static int unlink_quiet(const char *path)
{
	if (exists(path) == -ENOENT) {
		return 0;
	}
	int rc = fs_unlink(path);

	return (rc == -ENOENT) ? 0 : rc;
}

static int write_meta(struct meta *m)
{
	struct fs_file_t f;
	int rc;

	m->magic = META_MAGIC;
	m->version = META_VERSION;
	m->crc = meta_crc(m);

	fs_file_t_init(&f);
	rc = fs_open(&f, st.tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return -EIO;
	}
	ssize_t n = fs_write(&f, m, sizeof(*m));

	rc = n == (ssize_t)sizeof(*m) ? 0 : -EIO;
	if (rc == 0) {
		rc = fs_sync(&f);
	}
	int closed = fs_close(&f);

	rc = rc != 0 ? rc : closed;
	if (rc == 0) {
		rc = fs_rename(st.tmp, st.meta);
	}
	if (rc != 0) {
		(void)unlink_quiet(st.tmp);
		return -EIO;
	}
	return 0;
}

static int read_meta(struct meta *m)
{
	struct fs_file_t f;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, st.meta, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	ssize_t n = fs_read(&f, m, sizeof(*m));
	uint8_t extra;
	ssize_t more = fs_read(&f, &extra, 1);

	(void)fs_close(&f);
	return (n == (ssize_t)sizeof(*m) && more == 0) ? 0 : -EINVAL;
}

static int remove_meta(void)
{
	(void)unlink_quiet(st.tmp);
	return unlink_quiet(st.meta);
}

/* -- the slot --------------------------------------------------------------------- */

static int slot_read(uint32_t offset, uint8_t *buf, size_t len)
{
	return st.p->read(st.p->ctx, offset, buf, len) == 0 ? 0 : -EIO;
}

/*
 * Does the slot still hold the image the check accepted? The SHA-256 TLV and the
 * image's end, read from the slot. 1 yes, 0 no, -EIO unreadable.
 */
static int slot_holds(const struct meta *m)
{
	uint8_t hdr[MCUBOOT_IMAGE_HEADER_SIZE];
	uint8_t tlv[4];
	uint8_t hash[32];
	uint32_t off;
	uint32_t end;

	if (slot_read(0, hdr, sizeof(hdr)) != 0) {
		return -EIO;
	}
	if (sys_get_le32(hdr) != MCUBOOT_IMAGE_MAGIC) {
		return 0;
	}
	/* ih_hdr_size + ih_img_size + ih_protect_tlv_size */
	off = (uint32_t)MIN((uint64_t)sys_get_le16(&hdr[8]) + sys_get_le32(&hdr[12]) +
				    sys_get_le16(&hdr[10]),
			    (uint64_t)UINT32_MAX);
	if (off > m->size || m->size - off < sizeof(tlv)) {
		return 0;
	}
	if (slot_read(off, tlv, sizeof(tlv)) != 0) {
		return -EIO;
	}
	end = off + sys_get_le16(&tlv[2]);
	if (sys_get_le16(tlv) != MCUBOOT_TLV_INFO_MAGIC || end != m->size) {
		return 0;
	}
	for (off += sizeof(tlv); off + sizeof(tlv) <= end;) {
		if (slot_read(off, tlv, sizeof(tlv)) != 0) {
			return -EIO;
		}
		const uint16_t len = sys_get_le16(&tlv[2]);

		if (sys_get_le16(tlv) == MCUBOOT_TLV_SHA256 && len == sizeof(hash) &&
		    off + sizeof(tlv) + len <= end) {
			if (slot_read(off + sizeof(tlv), hash, sizeof(hash)) != 0) {
				return -EIO;
			}
			return memcmp(hash, m->image_hash, sizeof(hash)) == 0 ? 1 : 0;
		}
		off += sizeof(tlv) + len;
	}
	return 0;
}

static int recover(void)
{
	struct meta m;
	int rc = read_meta(&m);

	st.present = false;
	(void)unlink_quiet(st.tmp);
	if (rc == -ENOENT) {
		return 0;
	}
	if (rc != 0 || !meta_valid(&m)) {
		LOG_WRN("system image metadata unreadable, forgetting the upload");
		return remove_meta();
	}
	if (m.state == SYS_IMG_READY) {
		rc = slot_holds(&m);
		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			LOG_INF("slot 2 no longer holds upload %s, forgetting it", m.id);
			return remove_meta();
		}
	}
	st.m = m;
	st.present = true;
	return 0;
}

static bool platform_valid(const struct sys_img_platform *p)
{
	const uint32_t sector = p != NULL ? p->sector_size : 0U;

	return p != NULL && p->read != NULL && p->write != NULL && p->erase != NULL &&
	       p->slot_locked != NULL && sector > 0U && (sector & (sector - 1U)) == 0U &&
	       sector <= CONFIG_SYSTEM_IMAGE_STORE_SECTOR_MAX && p->trailer_bytes > 0U &&
	       p->trailer_bytes % sector == 0U && p->slot_size % sector == 0U &&
	       p->slot_size > p->trailer_bytes &&
	       p->image.header_size >= MCUBOOT_IMAGE_HEADER_SIZE;
}

/* -- state ------------------------------------------------------------------------ */

static bool matches(const char *id)
{
	return st.present && id != NULL && strcmp(id, st.m.id) == 0;
}

static void forget(void)
{
	st.present = false;
	st.job[0] = '\0';
	st.in_use = false;
	st.pending = false;
	st.verifying = false;
}

static void expire(int64_t now_ms)
{
	if (st.present && !st.in_use && !st.pending && !st.verifying &&
	    now_ms - st.last_activity >=
		    (int64_t)CONFIG_SYSTEM_IMAGE_STORE_EXPIRE_SECONDS * 1000) {
		LOG_INF("upload %s expired", st.m.id);
		if (remove_meta() == 0) {
			forget();
		}
	}
}

static void fill_view(struct sys_img_upload *out)
{
	const struct meta *m = &st.m;

	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	strcpy(out->id, m->id);
	strcpy(out->filename, m->filename);
	out->size_bytes = m->size;
	out->received_bytes = m->received;
	memcpy(out->sha256, m->sha256, sizeof(out->sha256));
	out->state = st.verifying ? SYS_IMG_VERIFYING : (enum sys_img_state)m->state;
	strcpy(out->active_job_id, st.job);
	out->in_use = st.in_use;
	out->chunk_pending = st.pending;
	out->has_image = m->has_image != 0U && m->state == SYS_IMG_READY;
	if (out->has_image) {
		strcpy(out->version, m->version_text);
		memcpy(out->image_hash, m->image_hash, sizeof(out->image_hash));
	}
	strcpy(out->error_code, m->error_code);
	strcpy(out->error_message, m->error_message);
}

static void copy_text(char *dst, size_t cap, const char *src)
{
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, cap - 1U);
	dst[cap - 1U] = '\0';
}

/* -- API -------------------------------------------------------------------------- */

int sys_img_init(const char *dir, const struct sys_img_platform *platform, int64_t now_ms)
{
	int rc;

	if (dir == NULL || dir[0] == '\0' ||
	    strlen(dir) + sizeof("/" NAME_TMP) > (size_t)PATH_MAX_LEN || !platform_valid(platform)) {
		return -EINVAL;
	}
	if (!lock_ready) {
		k_mutex_init(&st.lock);
		lock_ready = true;
	}

	k_mutex_lock(&st.lock, K_FOREVER);
	st.ready = false;
	forget();
	st.p = platform;
	snprintf(st.meta, sizeof(st.meta), "%s/" NAME_META, dir);
	snprintf(st.tmp, sizeof(st.tmp), "%s/" NAME_TMP, dir);

	rc = psa_crypto_init() == PSA_SUCCESS ? 0 : -EIO;
	if (rc == 0 && exists(dir) != 0) {
		rc = fs_mkdir(dir);
		rc = (rc == -EEXIST) ? 0 : rc;
	}
	if (rc == 0) {
		rc = recover();
	}
	if (rc == 0) {
		st.ready = true;
		st.last_activity = now_ms;
	} else {
		LOG_ERR("system image store in %s unavailable: %d", dir, rc);
		rc = -EIO;
	}
	k_mutex_unlock(&st.lock);

	return rc;
}

int sys_img_create(const char *filename, uint32_t size, const uint8_t sha256[32], int64_t now_ms,
		   struct sys_img_upload *out)
{
	const struct sys_img_platform *p = st.p;
	struct meta m;
	uint8_t random[8];
	int rc = 0;

	if (!lock_ready) {
		return -EAGAIN;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready) {
		rc = -EAGAIN;
		goto out;
	}
	expire(now_ms);

	if (filename == NULL || filename[0] == '\0' || strlen(filename) > SYS_IMG_FILENAME_MAX ||
	    size == 0U || sha256 == NULL) {
		rc = -EINVAL;
		goto out;
	}
	if (st.present &&
	    (st.in_use || st.pending || st.verifying || st.m.state != SYS_IMG_FAILED)) {
		rc = -EBUSY;
		goto out;
	}
	if (size > max_size(p)) {
		rc = -EFBIG;
		goto out;
	}
	if (p->slot_locked(p->ctx)) {
		rc = -EACCES;
		goto out;
	}
	/* A failed upload is replaced. */
	if (remove_meta() != 0) {
		rc = -EIO;
		goto out;
	}
	forget();
	/* Before the metadata exists: a stale swap request never meets a new upload. */
	if (p->erase(p->ctx, p->slot_size - p->trailer_bytes, p->trailer_bytes) != 0) {
		rc = -EIO;
		goto out;
	}

	memset(&m, 0, sizeof(m));
	sys_rand_get(random, sizeof(random));
	snprintf(m.id, sizeof(m.id), ID_PREFIX "%02x%02x%02x%02x%02x%02x%02x%02x", random[0],
		 random[1], random[2], random[3], random[4], random[5], random[6], random[7]);
	strcpy(m.filename, filename);
	m.size = size;
	m.state = SYS_IMG_RECEIVING;
	memcpy(m.sha256, sha256, sizeof(m.sha256));

	rc = write_meta(&m);
	if (rc != 0) {
		goto out;
	}
	st.m = m;
	st.present = true;
	st.last_activity = now_ms;
	fill_view(out);
out:
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_get(const char *id, int64_t now_ms, struct sys_img_upload *out)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else {
		fill_view(out);
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_current(int64_t now_ms, struct sys_img_upload *out)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !st.present) {
		rc = -ENOENT;
	} else {
		fill_view(out);
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_chunk_accept(const char *id, uint32_t offset, const uint8_t *data, size_t len,
			 int64_t now_ms)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.p->slot_locked(st.p->ctx)) {
		rc = -EACCES;
	} else if (st.m.state != SYS_IMG_RECEIVING || st.verifying) {
		rc = -EINVAL;
	} else if (st.pending) {
		rc = -EBUSY;
	} else if (offset != st.m.received) {
		rc = -ERANGE;
	} else if (len == 0U || data == NULL) {
		rc = -ENODATA;
	} else if (len > sizeof(staging)) {
		rc = -E2BIG;
	} else if (len > (size_t)(st.m.size - offset)) {
		rc = -EOVERFLOW;
	} else {
		memcpy(staging, data, len);
		st.pending = true;
		st.pending_offset = offset;
		st.pending_len = len;
		st.last_activity = now_ms;
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

void sys_img_chunk_discard(const char *id)
{
	if (!lock_ready) {
		return;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (matches(id)) {
		st.pending = false;
	}
	k_mutex_unlock(&st.lock);
}

/*
 * Can [offset, offset + len) of the slot be programmed to staging[0..len)? NOR
 * only clears bits: yes when every byte already there is erased, equal, or has
 * every bit the new byte needs. 1 yes, 0 no, -EIO unreadable.
 */
static int programmable(uint32_t offset, size_t len)
{
	for (size_t done = 0; done < len;) {
		const size_t n = MIN(sizeof(read_buf), len - done);

		if (slot_read(offset + done, read_buf, n) != 0) {
			return -EIO;
		}
		for (size_t i = 0; i < n; i++) {
			if ((read_buf[i] & staging[done + i]) != staging[done + i]) {
				return 0;
			}
		}
		done += n;
	}
	return 1;
}

/*
 * The staged chunk at @p offset into the slot; runs without the lock (the pending
 * flag holds the staging buffer and the upload still). @p intact is set to the
 * length of the upload still in the slot: @p offset, unless a repair erased the
 * sector's accepted bytes and could not write them back.
 */
static int write_chunk(uint32_t offset, size_t len, uint32_t *intact)
{
	const struct sys_img_platform *p = st.p;
	const uint32_t sector = p->sector_size;
	const uint32_t end = offset + (uint32_t)len;
	const uint32_t first_start = ROUND_UP(offset, sector);
	int rc;

	*intact = offset;
	/* The sector the chunk begins in the middle of: accepted bytes before offset. */
	if (first_start > offset) {
		const uint32_t head = MIN(first_start, end) - offset;

		rc = programmable(offset, head);
		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			const uint32_t base = first_start - sector;
			const uint32_t keep = offset - base;

			LOG_WRN("sector 0x%x cannot take the chunk at 0x%x, rewriting it", base,
				offset);
			if (slot_read(base, repair_buf, keep) != 0) {
				return -EIO;
			}
			/* From the erase on, a failure may have lost [base, offset). */
			if (p->erase(p->ctx, base, sector) != 0 ||
			    p->write(p->ctx, base, repair_buf, keep) != 0) {
				*intact = base;
				return -EIO;
			}
		}
	}
	/* Every sector that starts inside the chunk. */
	if (ROUND_UP(end, sector) > first_start &&
	    p->erase(p->ctx, first_start, ROUND_UP(end, sector) - first_start) != 0) {
		return -EIO;
	}
	if (p->write(p->ctx, offset, staging, len) != 0) {
		return -EIO;
	}
	for (size_t done = 0; done < len;) {
		const size_t n = MIN(sizeof(read_buf), len - done);

		if (slot_read(offset + done, read_buf, n) != 0 ||
		    memcmp(read_buf, &staging[done], n) != 0) {
			LOG_ERR("slot 2 read-back differs at 0x%x", offset + (uint32_t)done);
			return -EIO;
		}
		done += n;
	}
	return 0;
}

int sys_img_chunk_commit(const char *id, int64_t now_ms)
{
	struct meta next;
	uint32_t offset;
	uint32_t intact;
	size_t len;
	int rc;
	bool record = false;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		k_mutex_unlock(&st.lock);
		return -ENOENT;
	}
	if (!st.pending) {
		k_mutex_unlock(&st.lock);
		return -ENODATA;
	}
	offset = st.pending_offset;
	len = st.pending_len;
	next = st.m;
	k_mutex_unlock(&st.lock);

	rc = write_chunk(offset, len, &intact);
	if (rc == 0) {
		next.received = offset + (uint32_t)len;
		rc = write_meta(&next);
		record = rc == 0;
	} else if (intact < offset) {
		/*
		 * Accepted bytes were lost: the client must send them again. The state
		 * in RAM says so even if the record fails; after a reboot the metadata
		 * would claim them, and the check's SHA-256 finds the gap.
		 */
		LOG_ERR("slot 2 lost bytes from 0x%x, receiving again from there", intact);
		next.received = intact;
		(void)write_meta(&next);
		record = true;
	}

	k_mutex_lock(&st.lock, K_FOREVER);
	st.pending = false;
	if (record) {
		st.m = next;
	}
	st.last_activity = now_ms;
	k_mutex_unlock(&st.lock);

	return rc;
}

int sys_img_verify_begin(const char *id, int64_t now_ms)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.pending || st.in_use) {
		rc = -EBUSY;
	} else if (st.verifying || st.m.state == SYS_IMG_READY || st.m.received != st.m.size) {
		rc = -EINVAL;
	} else {
		st.verifying = true;
		st.last_activity = now_ms;
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

/* The check ended without an outcome: back to what the upload was before it. */
static void verify_abandon(bool cancelled, int64_t now_ms)
{
	k_mutex_lock(&st.lock, K_FOREVER);
	st.verifying = false;
	st.last_activity = now_ms;
	if (cancelled && st.m.state != SYS_IMG_RECEIVING) {
		struct meta next = st.m;

		next.state = SYS_IMG_RECEIVING;
		next.error_code[0] = '\0';
		next.error_message[0] = '\0';
		if (write_meta(&next) == 0) {
			st.m = next;
		}
	}
	k_mutex_unlock(&st.lock);
}

int sys_img_verify(const char *id, sys_img_cancelled_t cancelled, sys_img_progress_t progress,
		   void *ctx, int64_t now_ms)
{
	struct mcuboot_image_info info;
	struct meta next;
	const char *why = NULL;
	uint32_t done = 0;
	int rc;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id) || !st.verifying) {
		k_mutex_unlock(&st.lock);
		return -ENOENT;
	}
	next = st.m;
	k_mutex_unlock(&st.lock);

	mcuboot_image_check_begin(&check, &st.p->image, next.size);
	while (done < next.size) {
		if (cancelled != NULL && cancelled(ctx)) {
			mcuboot_image_check_abort(&check);
			verify_abandon(true, now_ms);
			return -ECANCELED;
		}
		const size_t n = MIN(sizeof(read_buf), (size_t)(next.size - done));

		if (slot_read(done, read_buf, n) != 0) {
			mcuboot_image_check_abort(&check);
			verify_abandon(false, now_ms);
			return -EIO;
		}
		mcuboot_image_check_feed(&check, read_buf, n);
		done += (uint32_t)n;
		if (progress != NULL) {
			progress(ctx, done, next.size);
		}
	}

	enum mcuboot_image_result result = mcuboot_image_check_end(&check, &info, &why);

	if (memcmp(info.sha256, next.sha256, sizeof(next.sha256)) != 0) {
		result = MCUBOOT_IMAGE_INVALID;
		why = "The file's SHA-256 does not match the one given when the upload was created";
	}

	if (result == MCUBOOT_IMAGE_OK) {
		next.state = SYS_IMG_READY;
		next.has_image = 1;
		copy_text(next.version_text, sizeof(next.version_text), info.version);
		next.major = info.major;
		next.minor = info.minor;
		next.revision = info.revision;
		next.build = info.build;
		memcpy(next.image_hash, info.image_hash, sizeof(next.image_hash));
		next.error_code[0] = '\0';
		next.error_message[0] = '\0';
	} else {
		next.state = SYS_IMG_FAILED;
		next.has_image = 0;
		next.version_text[0] = '\0';
		memset(next.image_hash, 0, sizeof(next.image_hash));
		copy_text(next.error_code, sizeof(next.error_code),
			  mcuboot_image_result_code(result));
		copy_text(next.error_message, sizeof(next.error_message), why);
	}

	k_mutex_lock(&st.lock, K_FOREVER);
	rc = write_meta(&next);
	st.verifying = false;
	st.last_activity = now_ms;
	if (rc == 0) {
		st.m = next;
	}
	k_mutex_unlock(&st.lock);

	return rc == 0 ? 0 : -EIO;
}

int sys_img_delete(const char *id)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.in_use || st.pending || st.verifying) {
		rc = -EBUSY;
	} else if (remove_meta() != 0) {
		rc = -EIO;
	} else {
		forget();
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_set_in_use(const char *id, bool in_use)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (in_use && st.m.state != SYS_IMG_READY) {
		rc = -EINVAL;
	} else {
		st.in_use = in_use;
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_set_active_job(const char *id, const char *job_id)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else {
		copy_text(st.job, sizeof(st.job), job_id);
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int sys_img_image_info(const char *id, struct sys_img_image *out)
{
	int rc = 0;

	if (!lock_ready) {
		return -ENOENT;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.m.state != SYS_IMG_READY || st.verifying || st.m.has_image == 0U) {
		rc = -EINVAL;
	} else if (out != NULL) {
		memset(out, 0, sizeof(*out));
		strcpy(out->version, st.m.version_text);
		out->major = st.m.major;
		out->minor = st.m.minor;
		out->revision = st.m.revision;
		out->build = st.m.build;
		memcpy(out->image_hash, st.m.image_hash, sizeof(out->image_hash));
		out->size_bytes = st.m.size;
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

void sys_img_tick(int64_t now_ms)
{
	if (!lock_ready) {
		return;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	k_mutex_unlock(&st.lock);
}

const char *sys_img_state_str(enum sys_img_state state)
{
	switch (state) {
	case SYS_IMG_RECEIVING:
		return "receiving";
	case SYS_IMG_VERIFYING:
		return "verifying";
	case SYS_IMG_READY:
		return "ready";
	case SYS_IMG_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}
