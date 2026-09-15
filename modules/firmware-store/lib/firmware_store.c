/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The staged image as two files. See include/firmware_store/firmware_store.h for
 * the contract and README.md for how it fits the system.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include <firmware_store/firmware_store.h>

LOG_MODULE_REGISTER(firmware_store, CONFIG_FIRMWARE_STORE_LOG_LEVEL);

#define ID_PREFIX      "upload_"
#define NAME_NAMESPACE "upload"
#define NAME_BIN       "upload.bin"
#define NAME_META      "upload.meta"
#define NAME_TMP       "upload.meta.tmp"
#define PATH_MAX_LEN   96

#define META_MAGIC   0x4d574643U /* "CFWM" */
#define META_VERSION 1U

/* The metadata file. Packed and zero-filled, so the CRC covers no stray padding. */
struct meta {
	uint32_t magic;
	uint16_t version;
	uint8_t state;
	uint8_t has_image;
	char id[FW_STORE_ID_LEN + 1];
	char filename[FW_STORE_FILENAME_MAX + 1];
	uint32_t size;
	uint32_t received;
	uint8_t sha256[32];
	char version_text[FW_IMAGE_TEXT_MAX + 1];
	char project_name[FW_IMAGE_TEXT_MAX + 1];
	char idf_version[FW_IMAGE_TEXT_MAX + 1];
	char layout_id[FW_STORE_LAYOUT_ID_MAX + 1];
	char host_protocol[FW_STORE_HOST_PROTOCOL_MAX + 1];
	uint32_t bootloader_bytes;
	uint32_t app_bytes;
	char error_code[FW_STORE_ERROR_CODE_MAX + 1];
	char error_message[FW_STORE_ERROR_MESSAGE_MAX + 1];
	uint32_t crc;
} __packed;

static struct {
	struct k_mutex lock;
	bool ready;
	char dir[PATH_MAX_LEN];
	char bin[PATH_MAX_LEN];
	char meta[PATH_MAX_LEN];
	char tmp[PATH_MAX_LEN];

	bool present;
	struct meta m;

	char job[FW_STORE_JOB_ID_MAX + 1];
	bool in_use;
	bool pending;
	bool verifying;
	bool reader_open;
	uint32_t pending_offset;
	size_t pending_len;
	int64_t last_activity;
} st;

static bool lock_ready;
static uint8_t staging[CONFIG_FIRMWARE_STORE_CHUNK_MAX];
static uint8_t read_buf[CONFIG_FIRMWARE_STORE_READ_BUF];
static struct fw_image_check check;
static struct fs_file_t reader;

/* -- files ------------------------------------------------------------------------ */

static uint32_t meta_crc(const struct meta *m)
{
	return crc32_ieee((const uint8_t *)m, offsetof(struct meta, crc));
}

static bool terminated(const char *s, size_t cap)
{
	return memchr(s, '\0', cap) != NULL;
}

static bool meta_valid(const struct meta *m)
{
	return m->magic == META_MAGIC && m->version == META_VERSION && m->crc == meta_crc(m) &&
	       (m->state == FW_UPLOAD_RECEIVING || m->state == FW_UPLOAD_READY ||
		m->state == FW_UPLOAD_FAILED) &&
	       terminated(m->id, sizeof(m->id)) && strlen(m->id) == FW_STORE_ID_LEN &&
	       strncmp(m->id, ID_PREFIX, strlen(ID_PREFIX)) == 0 &&
	       terminated(m->filename, sizeof(m->filename)) && m->filename[0] != '\0' &&
	       m->size > 0U && m->size <= CONFIG_FIRMWARE_STORE_MAX_BYTES &&
	       m->received <= m->size && terminated(m->version_text, sizeof(m->version_text)) &&
	       terminated(m->project_name, sizeof(m->project_name)) &&
	       terminated(m->idf_version, sizeof(m->idf_version)) &&
	       terminated(m->layout_id, sizeof(m->layout_id)) &&
	       terminated(m->host_protocol, sizeof(m->host_protocol)) &&
	       terminated(m->error_code, sizeof(m->error_code)) &&
	       terminated(m->error_message, sizeof(m->error_message));
}

static int as_storage_error(int rc)
{
	return rc == -ENOSPC ? -ENOSPC : -EIO;
}

static int unlink_quiet(const char *path)
{
	int rc = fs_unlink(path);

	return (rc == -ENOENT) ? 0 : rc;
}

static int write_all(struct fs_file_t *f, const uint8_t *data, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = fs_write(f, &data[done], len - done);

		if (n < 0) {
			return (int)n;
		}
		if (n == 0) {
			return -ENOSPC;
		}
		done += (size_t)n;
	}
	return 0;
}

static int write_meta(struct meta *m)
{
	struct fs_file_t f;
	int rc;

	m->magic = META_MAGIC;
	m->version = META_VERSION;
	m->crc = meta_crc(m);

	/*
	 * FS_O_TRUNC empties a leftover temporary file; no unlink first. Zephyr's
	 * fs_unlink() logs an error for a missing path, and before every chunk's
	 * metadata that was "failed to unlink path (-2)" once per chunk.
	 */
	fs_file_t_init(&f);
	rc = fs_open(&f, st.tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return as_storage_error(rc);
	}
	rc = write_all(&f, (const uint8_t *)m, sizeof(*m));
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
		return as_storage_error(rc);
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

static int file_size(const char *path, size_t *size)
{
	struct fs_dirent entry;
	int rc = fs_stat(path, &entry);

	if (rc == 0) {
		*size = entry.size;
	}
	return rc;
}

static int truncate_file(const char *path, uint32_t length)
{
	struct fs_file_t f;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_RDWR);
	if (rc != 0) {
		return rc;
	}
	rc = fs_truncate(&f, length);
	if (rc == 0) {
		rc = fs_sync(&f);
	}
	int closed = fs_close(&f);

	return rc != 0 ? rc : closed;
}

static int remove_files(void)
{
	int rc = unlink_quiet(st.bin);
	int rc_meta = unlink_quiet(st.meta);

	(void)unlink_quiet(st.tmp);
	return rc != 0 ? rc : rc_meta;
}

static int free_space(uint64_t *bytes, uint64_t *unit)
{
	struct fs_statvfs sv;
	int rc = fs_statvfs(st.dir, &sv);

	if (rc != 0) {
		return rc;
	}
	*unit = sv.f_frsize > 0U ? sv.f_frsize : 1U;
	*bytes = (uint64_t)sv.f_bfree * sv.f_frsize;
	return 0;
}

static bool room_for(uint64_t missing, int *error)
{
	uint64_t bytes;
	uint64_t unit;

	*error = free_space(&bytes, &unit);
	if (*error != 0) {
		*error = -EIO;
		return false;
	}
	const uint64_t need = ROUND_UP(missing, unit) +
			      (uint64_t)CONFIG_FIRMWARE_STORE_RESERVE_KIB * 1024U;

	if (bytes < need) {
		*error = -ENOSPC;
		return false;
	}
	return true;
}

/* The longest leftover name removed; the store's own names are far shorter. */
#define STRAY_NAME_MAX 64

/* Names of this store's namespace that are not its two files: leftovers. */
static int remove_strays(void)
{
	char names[4][STRAY_NAME_MAX];
	size_t found;

	do {
		struct fs_dir_t dir;
		struct fs_dirent entry;
		int rc;

		found = 0;
		fs_dir_t_init(&dir);
		rc = fs_opendir(&dir, st.dir);
		if (rc != 0) {
			return rc;
		}
		while (found < ARRAY_SIZE(names)) {
			rc = fs_readdir(&dir, &entry);
			if (rc != 0 || entry.name[0] == '\0') {
				break;
			}
			if (strncmp(entry.name, NAME_NAMESPACE, strlen(NAME_NAMESPACE)) == 0 &&
			    strcmp(entry.name, NAME_BIN) != 0 && strcmp(entry.name, NAME_META) != 0 &&
			    strlen(entry.name) < STRAY_NAME_MAX) {
				strcpy(names[found], entry.name);
				found++;
			}
		}
		(void)fs_closedir(&dir);
		if (rc != 0) {
			return rc;
		}

		for (size_t i = 0; i < found; i++) {
			char path[PATH_MAX_LEN + STRAY_NAME_MAX + 2];
			const size_t dir_len = strlen(st.dir);
			const size_t name_len = strlen(names[i]);

			if (dir_len + 1U + name_len >= sizeof(path)) {
				continue;
			}
			memcpy(path, st.dir, dir_len);
			path[dir_len] = '/';
			memcpy(&path[dir_len + 1U], names[i], name_len + 1U);
			LOG_WRN("removing leftover %s", path);
			rc = unlink_quiet(path);
			if (rc != 0) {
				return rc;
			}
		}
	} while (found == ARRAY_SIZE(names));

	return 0;
}

static int recover(void)
{
	struct meta m;
	size_t have = 0;
	int rc = read_meta(&m);

	st.present = false;
	if (rc == -ENOENT) {
		return unlink_quiet(st.bin);
	}
	if (rc != 0 || !meta_valid(&m)) {
		LOG_WRN("upload metadata unreadable, removing the staged image");
		return remove_files();
	}

	rc = file_size(st.bin, &have);
	if (rc == -ENOENT) {
		have = 0;
	} else if (rc != 0) {
		return rc;
	}

	bool changed = false;

	if (m.state != FW_UPLOAD_RECEIVING && have != m.size) {
		LOG_WRN("staged image is %zu of %u bytes; receiving again", have, m.size);
		m.state = FW_UPLOAD_RECEIVING;
		m.has_image = 0;
		m.error_code[0] = '\0';
		m.error_message[0] = '\0';
		changed = true;
	}
	if (m.state == FW_UPLOAD_RECEIVING) {
		if (have > m.received) {
			/* A chunk reached the file but not the metadata. */
			rc = truncate_file(st.bin, m.received);
			if (rc != 0) {
				return rc;
			}
		} else if (have < m.received) {
			m.received = (uint32_t)have;
			changed = true;
		}
	}
	if (changed) {
		rc = write_meta(&m);
		if (rc != 0) {
			return rc;
		}
	}

	st.m = m;
	st.present = true;
	return 0;
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
	if (st.present && !st.in_use && !st.pending && !st.verifying && !st.reader_open &&
	    now_ms - st.last_activity >= (int64_t)CONFIG_FIRMWARE_STORE_EXPIRE_SECONDS * 1000) {
		LOG_INF("upload %s expired", st.m.id);
		if (remove_files() == 0) {
			forget();
		}
	}
}

static void fill_view(struct fw_upload *out)
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
	out->state = st.verifying ? FW_UPLOAD_VERIFYING : (enum fw_upload_state)m->state;
	strcpy(out->active_job_id, st.job);
	out->in_use = st.in_use;
	out->chunk_pending = st.pending;
	out->has_image = m->has_image != 0U && m->state == FW_UPLOAD_READY;
	if (out->has_image) {
		strcpy(out->version, m->version_text);
		strcpy(out->project_name, m->project_name);
		strcpy(out->idf_version, m->idf_version);
		strcpy(out->layout_id, m->layout_id);
		strcpy(out->host_protocol, m->host_protocol);
		out->bootloader_bytes = m->bootloader_bytes;
		out->app_bytes = m->app_bytes;
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

int fw_store_init(const char *dir, int64_t now_ms)
{
	int rc;

	if (dir == NULL || dir[0] == '\0' ||
	    strlen(dir) + sizeof("/" NAME_TMP) > (size_t)PATH_MAX_LEN) {
		return -EINVAL;
	}
	if (!lock_ready) {
		k_mutex_init(&st.lock);
		lock_ready = true;
	}

	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.reader_open) {
		(void)fs_close(&reader);
	}
	st.ready = false;
	st.reader_open = false;
	forget();
	strcpy(st.dir, dir);
	snprintf(st.bin, sizeof(st.bin), "%s/" NAME_BIN, dir);
	snprintf(st.meta, sizeof(st.meta), "%s/" NAME_META, dir);
	snprintf(st.tmp, sizeof(st.tmp), "%s/" NAME_TMP, dir);

	rc = psa_crypto_init() == PSA_SUCCESS ? 0 : -EIO;
	if (rc == 0) {
		rc = fs_mkdir(dir);
		rc = (rc == -EEXIST) ? 0 : rc;
	}
	if (rc == 0) {
		rc = remove_strays();
	}
	if (rc == 0) {
		rc = recover();
	}
	if (rc == 0) {
		st.ready = true;
		st.last_activity = now_ms;
	} else {
		LOG_ERR("firmware store in %s unavailable: %d", dir, rc);
		rc = -EIO;
	}
	k_mutex_unlock(&st.lock);

	return rc;
}

int fw_store_create(const char *filename, uint32_t size, const uint8_t sha256[32], int64_t now_ms,
		    struct fw_upload *out)
{
	struct meta m;
	uint8_t random[8];
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready) {
		rc = -EAGAIN;
		goto out;
	}
	expire(now_ms);

	if (filename == NULL || filename[0] == '\0' ||
	    strlen(filename) > FW_STORE_FILENAME_MAX || size == 0U || sha256 == NULL) {
		rc = -EINVAL;
		goto out;
	}
	if (st.present && (st.in_use || st.pending || st.verifying || st.reader_open ||
			   st.m.state != FW_UPLOAD_FAILED)) {
		rc = -EBUSY;
		goto out;
	}
	if (size > CONFIG_FIRMWARE_STORE_MAX_BYTES) {
		rc = -EFBIG;
		goto out;
	}
	/* A failed upload is replaced, and a data file without metadata never counts. */
	rc = remove_files();
	if (rc != 0) {
		rc = -EIO;
		goto out;
	}
	forget();
	/* The only free-space check of an upload: its size is known here (see write_chunk()). */
	if (!room_for(size, &rc)) {
		goto out;
	}

	memset(&m, 0, sizeof(m));
	sys_rand_get(random, sizeof(random));
	snprintf(m.id, sizeof(m.id), ID_PREFIX "%02x%02x%02x%02x%02x%02x%02x%02x", random[0],
		 random[1], random[2], random[3], random[4], random[5], random[6], random[7]);
	strcpy(m.filename, filename);
	m.size = size;
	m.state = FW_UPLOAD_RECEIVING;
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

int fw_store_get(const char *id, int64_t now_ms, struct fw_upload *out)
{
	int rc = 0;

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

int fw_store_chunk_accept(const char *id, uint32_t offset, const uint8_t *data, size_t len,
			  int64_t now_ms)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.m.state != FW_UPLOAD_RECEIVING || st.verifying) {
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

void fw_store_chunk_discard(const char *id)
{
	k_mutex_lock(&st.lock, K_FOREVER);
	if (matches(id)) {
		st.pending = false;
	}
	k_mutex_unlock(&st.lock);
}

/*
 * The staged chunk at @p offset; runs without the lock (the pending flag holds the
 * staging buffer and the upload still).
 *
 * No free-space check here: fw_store_create() made it once for the whole size. On
 * LittleFS fs_statvfs() is lfs_fs_size(), a walk of every block of every file, so a
 * check per chunk made each chunk slower than the last as the image grew. Space taken
 * by someone else meanwhile still ends the write: LittleFS returns -ENOSPC.
 */
static int write_chunk(uint32_t offset, size_t len)
{
	struct fs_file_t f;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, st.bin, FS_O_CREATE | FS_O_RDWR);
	if (rc != 0) {
		return as_storage_error(rc);
	}

	rc = fs_seek(&f, 0, FS_SEEK_END);
	off_t have = rc == 0 ? fs_tell(&f) : -1;

	if (have < 0 || have < (off_t)offset) {
		/* Committed bytes are missing: nothing sensible to append to. */
		rc = -EIO;
	} else if (have > (off_t)offset) {
		rc = fs_truncate(&f, offset);
	}
	if (rc == 0) {
		rc = fs_seek(&f, offset, FS_SEEK_SET);
	}
	if (rc == 0) {
		rc = write_all(&f, staging, len);
	}
	if (rc == 0) {
		rc = fs_sync(&f);
	}
	if (rc != 0 && have >= (off_t)offset) {
		(void)fs_truncate(&f, offset);
	}
	int closed = fs_close(&f);

	rc = rc != 0 ? rc : closed;
	return rc == 0 ? 0 : as_storage_error(rc);
}

int fw_store_chunk_commit(const char *id, int64_t now_ms)
{
	struct meta next;
	uint32_t offset;
	size_t len;
	int rc;

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

	rc = write_chunk(offset, len);
	if (rc == 0) {
		next.received = offset + (uint32_t)len;
		rc = write_meta(&next);
		if (rc != 0) {
			(void)truncate_file(st.bin, offset);
		}
	}

	k_mutex_lock(&st.lock, K_FOREVER);
	st.pending = false;
	if (rc == 0) {
		st.m = next;
	}
	st.last_activity = now_ms;
	k_mutex_unlock(&st.lock);

	return rc;
}

int fw_store_verify_begin(const char *id, int64_t now_ms)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.ready) {
		expire(now_ms);
	}
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.pending || st.in_use) {
		rc = -EBUSY;
	} else if (st.verifying || st.m.state == FW_UPLOAD_READY || st.m.received != st.m.size) {
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
	if (cancelled && st.m.state != FW_UPLOAD_RECEIVING) {
		struct meta next = st.m;

		next.state = FW_UPLOAD_RECEIVING;
		next.error_code[0] = '\0';
		next.error_message[0] = '\0';
		if (write_meta(&next) == 0) {
			st.m = next;
		}
	}
	k_mutex_unlock(&st.lock);
}

int fw_store_verify(const char *id, fw_store_cancelled_t cancelled, fw_store_progress_t progress,
		    void *ctx, int64_t now_ms)
{
	struct fw_image_info info;
	struct fs_file_t f;
	struct meta next;
	const char *why = NULL;
	uint32_t done = 0;
	int rc;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id) || !st.verifying) {
		k_mutex_unlock(&st.lock);
		return -ENOENT;
	}
	next = st.m;
	k_mutex_unlock(&st.lock);

	fs_file_t_init(&f);
	rc = fs_open(&f, st.bin, FS_O_READ);
	if (rc != 0) {
		verify_abandon(false, now_ms);
		return -EIO;
	}

	fw_image_check_begin(&check, next.size);
	while (done < next.size) {
		if (cancelled != NULL && cancelled(ctx)) {
			fw_image_check_abort(&check);
			(void)fs_close(&f);
			verify_abandon(true, now_ms);
			return -ECANCELED;
		}
		ssize_t n = fs_read(&f, read_buf, MIN(sizeof(read_buf), (size_t)(next.size - done)));

		if (n < 0) {
			fw_image_check_abort(&check);
			(void)fs_close(&f);
			verify_abandon(false, now_ms);
			return -EIO;
		}
		if (n == 0) {
			break;
		}
		fw_image_check_feed(&check, read_buf, (size_t)n);
		done += (uint32_t)n;
		if (progress != NULL) {
			progress(ctx, done, next.size);
		}
	}
	(void)fs_close(&f);

	enum fw_image_result result = fw_image_check_end(&check, &info, &why);

	if (done == next.size && memcmp(info.sha256, next.sha256, sizeof(next.sha256)) != 0) {
		result = FW_IMAGE_INVALID;
		why = "The file's SHA-256 does not match the one given when the upload was created";
	}

	if (result == FW_IMAGE_OK) {
		next.state = FW_UPLOAD_READY;
		next.has_image = 1;
		copy_text(next.version_text, sizeof(next.version_text), info.version);
		copy_text(next.project_name, sizeof(next.project_name), info.project_name);
		copy_text(next.idf_version, sizeof(next.idf_version), info.idf_version);
		copy_text(next.layout_id, sizeof(next.layout_id), info.layout_id);
		copy_text(next.host_protocol, sizeof(next.host_protocol), info.host_protocol);
		next.bootloader_bytes = info.bootloader_bytes;
		next.app_bytes = info.app_bytes;
		next.error_code[0] = '\0';
		next.error_message[0] = '\0';
	} else {
		next.state = FW_UPLOAD_FAILED;
		next.has_image = 0;
		copy_text(next.error_code, sizeof(next.error_code), fw_image_result_code(result));
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

int fw_store_delete(const char *id)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.in_use || st.reader_open || st.pending || st.verifying) {
		rc = -EBUSY;
	} else if (remove_files() != 0) {
		rc = -EIO;
	} else {
		forget();
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int fw_store_set_in_use(const char *id, bool in_use)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (in_use && st.m.state != FW_UPLOAD_READY) {
		rc = -EINVAL;
	} else {
		st.in_use = in_use;
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int fw_store_set_active_job(const char *id, const char *job_id)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else {
		copy_text(st.job, sizeof(st.job), job_id);
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

void fw_store_tick(int64_t now_ms)
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

int fw_store_image_open(const char *id, uint32_t *size)
{
	int rc = 0;

	k_mutex_lock(&st.lock, K_FOREVER);
	if (!st.ready || !matches(id)) {
		rc = -ENOENT;
	} else if (st.m.state != FW_UPLOAD_READY || st.verifying) {
		rc = -EINVAL;
	} else if (st.reader_open) {
		rc = -EBUSY;
	} else {
		fs_file_t_init(&reader);
		if (fs_open(&reader, st.bin, FS_O_READ) != 0) {
			rc = -EIO;
		} else {
			st.reader_open = true;
			if (size != NULL) {
				*size = st.m.size;
			}
		}
	}
	k_mutex_unlock(&st.lock);
	return rc;
}

int fw_store_image_read(uint32_t offset, uint8_t *buf, size_t len)
{
	size_t done = 0;

	if (!st.reader_open) {
		return -EBADF;
	}
	if (fs_seek(&reader, offset, FS_SEEK_SET) != 0) {
		return -EIO;
	}
	while (done < len) {
		ssize_t n = fs_read(&reader, &buf[done], len - done);

		if (n < 0) {
			return -EIO;
		}
		if (n == 0) {
			break;
		}
		done += (size_t)n;
	}
	return (int)done;
}

void fw_store_image_close(void)
{
	if (!lock_ready) {
		return;
	}
	k_mutex_lock(&st.lock, K_FOREVER);
	if (st.reader_open) {
		(void)fs_close(&reader);
		st.reader_open = false;
	}
	k_mutex_unlock(&st.lock);
}

const char *fw_upload_state_str(enum fw_upload_state state)
{
	switch (state) {
	case FW_UPLOAD_RECEIVING:
		return "receiving";
	case FW_UPLOAD_VERIFYING:
		return "verifying";
	case FW_UPLOAD_READY:
		return "ready";
	case FW_UPLOAD_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}
