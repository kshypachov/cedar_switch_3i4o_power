/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * log-store implementation. Design in include/log_store/log_store.h and
 * ../README.md.
 *
 * Nothing in this file logs: the Zephyr log backend is one of the producers.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <log_store/log_store.h>

/*
 * Record layout in a ring, little-endian, 4-byte aligned:
 *
 *   0  u16 total length (header, module, text, padding, trailer)
 *   2  u8  level
 *   3  u8  flags: kind (bits 0-2), has_module (6), truncated (7)
 *   4  u8  module length
 *   5  u8  source
 *   6  u16 text length
 *   8  u32 generation
 *  12  u64 seq
 *  20  u64 uptime_ms
 *  28  module bytes, text bytes, zero padding
 *  end-4  u16 total length, u16 TRAILER_MAGIC
 */
#define HDR_SIZE       28U
#define TRAILER_SIZE   4U
#define TRAILER_MAGIC  0xC3D5U
#define RECORD_MIN     (ROUND_UP(HDR_SIZE, 4U) + TRAILER_SIZE)
#define RECORD_MAX                                                                                 \
	(ROUND_UP(HDR_SIZE + LOG_STORE_MODULE_MAX + LOG_STORE_TEXT_MAX, 4U) + TRAILER_SIZE)

#define FLAG_KIND_MASK  0x07U
#define FLAG_HAS_MODULE BIT(6)
#define FLAG_TRUNCATED  BIT(7)

#define CURSOR_VERSION   1U
#define CURSOR_RAW_LEN   29U /* version 1, boot 4, filter 4, pos 8+8, check 4 */
#define CURSOR_TEXT_LEN  39U /* unpadded base64url of 29 bytes */
#define FNV32_INIT       0x811C9DC5U
#define FNV32_PRIME      0x01000193U

#define STM32_RING_BYTES (CONFIG_LOG_STORE_STM32_KIB * 1024U)
#define ESP32_RING_BYTES (CONFIG_LOG_STORE_ESP32_KIB * 1024U)

BUILD_ASSERT(IS_POWER_OF_TWO(CONFIG_LOG_STORE_STM32_KIB), "STM32 ring must be a power of two");
BUILD_ASSERT(IS_POWER_OF_TWO(CONFIG_LOG_STORE_ESP32_KIB), "ESP32 ring must be a power of two");
BUILD_ASSERT(RECORD_MAX <= UINT16_MAX);
BUILD_ASSERT(CURSOR_TEXT_LEN < LOG_STORE_CURSOR_MAX);

struct ring {
	uint8_t *buf;
	uint32_t size;
	/* Absolute byte positions: bytes ever written, and the oldest record. */
	uint64_t head;
	uint64_t tail;
	uint32_t records;
	uint64_t appended;
	uint64_t overwritten;
};

static uint8_t stm32_buf[STM32_RING_BYTES];
static uint8_t esp32_buf[ESP32_RING_BYTES];

static struct ring rings[LOG_STORE_SOURCE_COUNT] = {
	[LOG_STORE_STM32] = {.buf = stm32_buf, .size = STM32_RING_BYTES},
	[LOG_STORE_ESP32] = {.buf = esp32_buf, .size = ESP32_RING_BYTES},
};

static K_MUTEX_DEFINE(store_lock);
static uint64_t next_seq = 1;
static bool initialized;
static atomic_t panicked;

/* Loss counters are touched from a panicking backend, possibly in a fault
 * handler, so they have a spinlock of their own and never the mutex. */
static struct k_spinlock loss_lock;
static uint64_t losses[LOG_STORE_SOURCE_COUNT][LOG_STORE_LOSS_COUNT];

static struct log_store_timing timing;

/* -- ring bytes ----------------------------------------------------------- */

static void ring_put(struct ring *r, uint64_t abs, const uint8_t *src, size_t n)
{
	uint32_t off = (uint32_t)(abs & (r->size - 1U));
	size_t first = MIN(n, (size_t)(r->size - off));

	memcpy(&r->buf[off], src, first);
	if (n > first) {
		memcpy(r->buf, &src[first], n - first);
	}
}

static void ring_get(const struct ring *r, uint64_t abs, uint8_t *dst, size_t n)
{
	uint32_t off = (uint32_t)(abs & (r->size - 1U));
	size_t first = MIN(n, (size_t)(r->size - off));

	memcpy(dst, &r->buf[off], first);
	if (n > first) {
		memcpy(&dst[first], r->buf, n - first);
	}
}

static bool length_sane(uint32_t len)
{
	return len >= RECORD_MIN && len <= RECORD_MAX && (len & 3U) == 0U;
}

/* -- timing ---------------------------------------------------------------- */

static uint32_t cycles_us(uint32_t from, uint32_t to)
{
	return (uint32_t)k_cyc_to_us_floor32(to - from);
}

static uint32_t lock_store(uint32_t *wait_us)
{
	uint32_t t0 = k_cycle_get_32();

	(void)k_mutex_lock(&store_lock, K_FOREVER);

	uint32_t t1 = k_cycle_get_32();

	if (wait_us != NULL) {
		*wait_us = cycles_us(t0, t1);
	}
	return t1;
}

static void note_max(uint32_t *max, uint32_t value)
{
	if (value > *max) {
		*max = value;
	}
}

/* -- text helpers ---------------------------------------------------------- */

size_t log_store_utf8_cut(const char *text, size_t len, size_t cap)
{
	size_t n = MIN(len, cap);

	for (size_t back = 1; back <= 4U && back <= n; back++) {
		uint8_t c = (uint8_t)text[n - back];
		size_t need;

		if ((c & 0xC0U) == 0x80U) {
			continue;
		}
		if (c < 0x80U) {
			need = 1;
		} else if ((c & 0xE0U) == 0xC0U) {
			need = 2;
		} else if ((c & 0xF0U) == 0xE0U) {
			need = 3;
		} else if ((c & 0xF8U) == 0xF0U) {
			need = 4;
		} else {
			need = 1;
		}
		return (back < need) ? n - back : n;
	}

	return n;
}

/* strnlen() is POSIX, not C17, and the host libc of the sim tier hides it. */
static size_t bounded_len(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0') {
		n++;
	}
	return n;
}

static uint32_t fnv1a(uint32_t h, const void *data, size_t n)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= FNV32_PRIME;
	}
	return h;
}

/* -- init, append, counters ----------------------------------------------- */

int log_store_init(void)
{
	(void)k_mutex_lock(&store_lock, K_FOREVER);
	for (size_t s = 0; s < ARRAY_SIZE(rings); s++) {
		rings[s].head = 0;
		rings[s].tail = 0;
		rings[s].records = 0;
		rings[s].appended = 0;
		rings[s].overwritten = 0;
	}
	next_seq = 1;
	memset(&timing, 0, sizeof(timing));
	atomic_set(&panicked, 0);
	initialized = true;
	k_mutex_unlock(&store_lock);

	k_spinlock_key_t key = k_spin_lock(&loss_lock);

	memset(losses, 0, sizeof(losses));
	k_spin_unlock(&loss_lock, key);

	return 0;
}

bool log_store_ready(void)
{
	return initialized;
}

void log_store_panic(void)
{
	atomic_set(&panicked, 1);
}

int log_store_append(const struct log_store_entry *e)
{
	uint8_t hdr[HDR_SIZE];
	uint8_t trailer[TRAILER_SIZE];
	static const uint8_t zeros[4];
	size_t tlen;
	size_t mlen = 0;
	bool truncated;
	uint32_t total;
	uint32_t wait_us;
	uint32_t t1;

	if (atomic_get(&panicked) != 0 || !initialized) {
		return -EAGAIN;
	}
	if (e == NULL || e->source >= LOG_STORE_SOURCE_COUNT || e->level > LOG_STORE_LEVEL_UNKNOWN ||
	    e->kind > LOG_STORE_KIND_GAP || (e->text == NULL && e->text_len > 0U)) {
		return -EINVAL;
	}

	tlen = e->text_len;
	truncated = e->truncated;
	if (tlen > LOG_STORE_TEXT_MAX) {
		tlen = log_store_utf8_cut(e->text, e->text_len, LOG_STORE_TEXT_MAX);
		truncated = true;
	}
	if (e->module != NULL) {
		mlen = e->module_len;
		if (mlen > LOG_STORE_MODULE_MAX) {
			mlen = log_store_utf8_cut(e->module, e->module_len, LOG_STORE_MODULE_MAX);
		}
	}

	total = ROUND_UP(HDR_SIZE + mlen + tlen, 4U) + TRAILER_SIZE;

	sys_put_le16((uint16_t)total, &hdr[0]);
	hdr[2] = (uint8_t)e->level;
	hdr[3] = (uint8_t)e->kind | (e->module != NULL ? FLAG_HAS_MODULE : 0U) |
		 (truncated ? FLAG_TRUNCATED : 0U);
	hdr[4] = (uint8_t)mlen;
	hdr[5] = (uint8_t)e->source;
	sys_put_le16((uint16_t)tlen, &hdr[6]);
	sys_put_le32(e->generation, &hdr[8]);
	sys_put_le64(e->uptime_ms, &hdr[20]);
	sys_put_le16((uint16_t)total, &trailer[0]);
	sys_put_le16(TRAILER_MAGIC, &trailer[2]);

	t1 = lock_store(&wait_us);

	if (atomic_get(&panicked) != 0) {
		k_mutex_unlock(&store_lock);
		return -EAGAIN;
	}

	struct ring *r = &rings[e->source];

	/* Push out the oldest records until the new one fits. */
	while (r->head + total - r->tail > r->size) {
		uint8_t lb[2];
		uint32_t len;

		ring_get(r, r->tail, lb, sizeof(lb));
		len = sys_get_le16(lb);
		if (!length_sane(len)) {
			/* Cannot happen short of memory corruption; start the
			 * ring over rather than walk garbage. */
			r->overwritten += r->records;
			r->records = 0;
			r->tail = r->head;
			break;
		}
		r->tail += len;
		r->records--;
		r->overwritten++;
	}

	sys_put_le64(next_seq, &hdr[12]);
	next_seq++;

	uint64_t pos = r->head;

	ring_put(r, pos, hdr, HDR_SIZE);
	pos += HDR_SIZE;
	if (mlen > 0U) {
		ring_put(r, pos, (const uint8_t *)e->module, mlen);
		pos += mlen;
	}
	if (tlen > 0U) {
		ring_put(r, pos, (const uint8_t *)e->text, tlen);
		pos += tlen;
	}
	ring_put(r, pos, zeros, total - TRAILER_SIZE - HDR_SIZE - mlen - tlen);
	ring_put(r, r->head + total - TRAILER_SIZE, trailer, TRAILER_SIZE);

	r->head += total;
	r->records++;
	r->appended++;

	note_max(&timing.append_wait_max_us, wait_us);
	note_max(&timing.append_hold_max_us, cycles_us(t1, k_cycle_get_32()));
	k_mutex_unlock(&store_lock);

	return 0;
}

void log_store_count_loss(enum log_store_source source, enum log_store_loss loss, uint32_t n)
{
	if (source >= LOG_STORE_SOURCE_COUNT || loss >= LOG_STORE_LOSS_COUNT) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&loss_lock);

	losses[source][loss] += n;
	k_spin_unlock(&loss_lock, key);
}

uint64_t log_store_dropped(uint8_t sources)
{
	uint64_t sum = 0;
	k_spinlock_key_t key = k_spin_lock(&loss_lock);

	for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
		if ((sources & LOG_STORE_SOURCE_BIT(s)) == 0U) {
			continue;
		}
		for (size_t l = 0; l < LOG_STORE_LOSS_COUNT; l++) {
			sum += losses[s][l];
		}
	}
	k_spin_unlock(&loss_lock, key);

	return sum;
}

void log_store_get_stats(enum log_store_source source, struct log_store_stats *out)
{
	memset(out, 0, sizeof(*out));
	if (source >= LOG_STORE_SOURCE_COUNT) {
		return;
	}

	(void)k_mutex_lock(&store_lock, K_FOREVER);
	const struct ring *r = &rings[source];

	out->appended = r->appended;
	out->overwritten = r->overwritten;
	out->used_bytes = (uint32_t)(r->head - r->tail);
	out->capacity_bytes = r->size;
	out->records = r->records;
	k_mutex_unlock(&store_lock);

	k_spinlock_key_t key = k_spin_lock(&loss_lock);

	memcpy(out->lost, losses[source], sizeof(out->lost));
	k_spin_unlock(&loss_lock, key);
}

void log_store_get_timing(struct log_store_timing *out)
{
	(void)k_mutex_lock(&store_lock, K_FOREVER);
	*out = timing;
	k_mutex_unlock(&store_lock);
}

uint64_t log_store_next_seq(void)
{
	uint64_t seq;

	(void)k_mutex_lock(&store_lock, K_FOREVER);
	seq = next_seq;
	k_mutex_unlock(&store_lock);

	return seq;
}

/* -- reading --------------------------------------------------------------- */

/* Copy the record whose header is @p hdr, at @p pos, into @p out. Lock held. */
static void copy_record(const struct ring *r, uint64_t pos, const uint8_t hdr[HDR_SIZE],
			struct log_store_record *out)
{
	uint8_t flags = hdr[3];
	size_t mlen = MIN((size_t)hdr[4], (size_t)LOG_STORE_MODULE_MAX);
	size_t tlen = MIN((size_t)sys_get_le16(&hdr[6]), (size_t)LOG_STORE_TEXT_MAX);

	out->source = (enum log_store_source)hdr[5];
	out->level = (enum log_store_level)hdr[2];
	out->kind = (enum log_store_kind)(flags & FLAG_KIND_MASK);
	out->truncated = (flags & FLAG_TRUNCATED) != 0U;
	out->has_module = (flags & FLAG_HAS_MODULE) != 0U;
	out->generation = sys_get_le32(&hdr[8]);
	out->seq = sys_get_le64(&hdr[12]);
	out->uptime_ms = sys_get_le64(&hdr[20]);
	ring_get(r, pos + HDR_SIZE, (uint8_t *)out->module, mlen);
	out->module[mlen] = '\0';
	ring_get(r, pos + HDR_SIZE + mlen, (uint8_t *)out->text, tlen);
	out->text[tlen] = '\0';
	out->text_len = (uint16_t)tlen;
}

static bool ascii_contains(const char *hay, size_t hay_len, const char *needle)
{
	size_t n = strlen(needle);

	if (n == 0U) {
		return true;
	}
	for (size_t i = 0; i + n <= hay_len; i++) {
		size_t k = 0;

		while (k < n) {
			char a = hay[i + k];
			char b = needle[k];

			if (a >= 'A' && a <= 'Z') {
				a = (char)(a - 'A' + 'a');
			}
			if (b >= 'A' && b <= 'Z') {
				b = (char)(b - 'A' + 'a');
			}
			if (a != b) {
				break;
			}
			k++;
		}
		if (k == n) {
			return true;
		}
	}
	return false;
}

bool log_store_filter_match(const struct log_store_filter *f, const struct log_store_record *rec)
{
	if (rec->source >= LOG_STORE_SOURCE_COUNT ||
	    (f->sources & LOG_STORE_SOURCE_BIT(rec->source)) == 0U) {
		return false;
	}
	if (f->min_level != LOG_STORE_LEVEL_NONE && rec->level >= LOG_STORE_LEVEL_DEBUG &&
	    rec->level <= LOG_STORE_LEVEL_ERROR && rec->level < f->min_level) {
		return false;
	}
	if (f->has_module && (!rec->has_module || strcmp(f->module, rec->module) != 0)) {
		return false;
	}
	if (f->has_contains && !ascii_contains(rec->text, rec->text_len, f->contains)) {
		return false;
	}
	return true;
}

static void reader_init(struct log_store_reader *reader, const struct log_store_filter *filter)
{
	memset(reader, 0, sizeof(*reader));
	reader->filter = *filter;
}

void log_store_reader_at(struct log_store_reader *reader, const struct log_store_filter *filter,
			 const uint64_t pos[LOG_STORE_SOURCE_COUNT])
{
	reader_init(reader, filter);

	(void)k_mutex_lock(&store_lock, K_FOREVER);
	for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
		const struct ring *r = &rings[s];
		uint64_t p = pos[s];

		if (p < r->tail) {
			p = r->tail;
			if ((filter->sources & LOG_STORE_SOURCE_BIT(s)) != 0U) {
				reader->gap = true;
			}
		}
		if (p > r->head) {
			p = r->head;
		}
		reader->pos[s] = p;
	}
	k_mutex_unlock(&store_lock);
}

void log_store_reader_tail(struct log_store_reader *reader, const struct log_store_filter *filter,
			   uint32_t count, uint32_t scan_budget)
{
	uint64_t bp[LOG_STORE_SOURCE_COUNT];
	bool done[LOG_STORE_SOURCE_COUNT];
	struct log_store_record rec;
	uint32_t matched = 0;
	uint32_t scanned = 0;

	reader_init(reader, filter);

	(void)k_mutex_lock(&store_lock, K_FOREVER);
	for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
		bp[s] = rings[s].head;
		done[s] = (filter->sources & LOG_STORE_SOURCE_BIT(s)) == 0U;
	}
	k_mutex_unlock(&store_lock);

	/* Walk both rings backwards, newest first across them, until the
	 * count-th match. Everything visited is newer than everything not, so
	 * reading forwards from where the walk stopped returns exactly the
	 * newest `count` matches. */
	while (matched < count && scanned < scan_budget) {
		uint8_t hdr[LOG_STORE_SOURCE_COUNT][HDR_SIZE];
		uint64_t start[LOG_STORE_SOURCE_COUNT];
		int best = -1;
		uint64_t best_seq = 0;
		uint32_t t1 = lock_store(NULL);

		for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
			const struct ring *r = &rings[s];
			uint8_t tr[TRAILER_SIZE];
			uint32_t len;

			if (done[s]) {
				continue;
			}
			if (bp[s] <= r->tail) {
				bp[s] = MAX(bp[s], r->tail);
				done[s] = true;
				continue;
			}
			ring_get(r, bp[s] - TRAILER_SIZE, tr, TRAILER_SIZE);
			len = sys_get_le16(tr);
			if (sys_get_le16(&tr[2]) != TRAILER_MAGIC || !length_sane(len) ||
			    bp[s] - r->tail < len) {
				bp[s] = r->tail;
				done[s] = true;
				continue;
			}
			start[s] = bp[s] - len;
			ring_get(r, start[s], hdr[s], HDR_SIZE);

			uint64_t seq = sys_get_le64(&hdr[s][12]);

			if (best < 0 || seq > best_seq) {
				best = (int)s;
				best_seq = seq;
			}
		}

		if (best < 0) {
			k_mutex_unlock(&store_lock);
			break;
		}
		copy_record(&rings[best], start[best], hdr[best], &rec);
		bp[best] = start[best];
		note_max(&timing.read_hold_max_us, cycles_us(t1, k_cycle_get_32()));
		k_mutex_unlock(&store_lock);

		scanned++;
		if (log_store_filter_match(filter, &rec)) {
			matched++;
		}
	}

	memcpy(reader->pos, bp, sizeof(bp));
	reader->scanned = scanned;
}

enum log_store_read_result log_store_read(struct log_store_reader *reader,
					  struct log_store_record *out, uint32_t scan_budget)
{
	uint32_t scanned = 0;

	if (!initialized) {
		return LOG_STORE_READ_END;
	}

	if ((reader->filter.sources & LOG_STORE_SOURCES_ALL) == 0U) {
		/* Matches nothing: stand at the heads, so the cursor means "from now". */
		(void)k_mutex_lock(&store_lock, K_FOREVER);
		for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
			reader->pos[s] = rings[s].head;
		}
		k_mutex_unlock(&store_lock);
		return LOG_STORE_READ_END;
	}

	while (scanned < scan_budget) {
		uint8_t hdr[LOG_STORE_SOURCE_COUNT][HDR_SIZE];
		int best = -1;
		uint64_t best_seq = 0;
		uint32_t len;
		uint32_t t1 = lock_store(NULL);

		for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
			const struct ring *r = &rings[s];

			if ((reader->filter.sources & LOG_STORE_SOURCE_BIT(s)) == 0U) {
				continue;
			}
			if (reader->pos[s] < r->tail) {
				reader->pos[s] = r->tail;
				reader->gap = true;
			}
			if (reader->pos[s] >= r->head) {
				reader->pos[s] = r->head;
				continue;
			}
			ring_get(r, reader->pos[s], hdr[s], HDR_SIZE);
			if (!length_sane(sys_get_le16(hdr[s]))) {
				reader->pos[s] = r->head;
				continue;
			}

			uint64_t seq = sys_get_le64(&hdr[s][12]);

			if (best < 0 || seq < best_seq) {
				best = (int)s;
				best_seq = seq;
			}
		}

		if (best < 0 || (reader->upper_seq != 0U && best_seq >= reader->upper_seq)) {
			k_mutex_unlock(&store_lock);
			return LOG_STORE_READ_END;
		}

		len = sys_get_le16(hdr[best]);
		copy_record(&rings[best], reader->pos[best], hdr[best], out);
		reader->pos[best] += len;
		note_max(&timing.read_hold_max_us, cycles_us(t1, k_cycle_get_32()));
		k_mutex_unlock(&store_lock);

		scanned++;
		reader->scanned++;
		if (log_store_filter_match(&reader->filter, out)) {
			return LOG_STORE_READ_RECORD;
		}
	}

	return LOG_STORE_READ_BUDGET;
}

/* -- cursors --------------------------------------------------------------- */

static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64url_value(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '-') {
		return 62;
	}
	if (c == '_') {
		return 63;
	}
	return -1;
}

uint32_t log_store_boot_tag(const char *boot_id)
{
	return boot_id == NULL ? 0U : fnv1a(FNV32_INIT, boot_id, strlen(boot_id));
}

static uint32_t filter_digest(const struct log_store_filter *f)
{
	uint8_t head[3] = {f->sources & LOG_STORE_SOURCES_ALL, (uint8_t)f->min_level,
			   (uint8_t)((f->has_module ? 1U : 0U) | (f->has_contains ? 2U : 0U))};
	uint32_t h = fnv1a(FNV32_INIT, head, sizeof(head));

	if (f->has_module) {
		size_t n = bounded_len(f->module, LOG_STORE_MODULE_MAX);
		uint8_t len = (uint8_t)n;

		h = fnv1a(h, &len, 1);
		h = fnv1a(h, f->module, n);
	}
	if (f->has_contains) {
		size_t n = bounded_len(f->contains, LOG_STORE_CONTAINS_MAX);
		uint8_t len[2];

		sys_put_le16((uint16_t)n, len);
		h = fnv1a(h, len, sizeof(len));
		h = fnv1a(h, f->contains, n);
	}
	return h;
}

static uint32_t cursor_check(const uint8_t *raw)
{
	static const char salt[] = "cedar-log-cursor";

	return fnv1a(fnv1a(FNV32_INIT, salt, sizeof(salt) - 1U), raw, CURSOR_RAW_LEN - 4U);
}

int log_store_cursor_encode(const struct log_store_reader *reader, uint32_t boot_tag, char *out,
			    size_t cap)
{
	uint8_t raw[CURSOR_RAW_LEN];
	size_t o = 0;

	if (cap < CURSOR_TEXT_LEN + 1U) {
		return -ENOSPC;
	}

	raw[0] = CURSOR_VERSION;
	sys_put_le32(boot_tag, &raw[1]);
	sys_put_le32(filter_digest(&reader->filter), &raw[5]);
	sys_put_le64(reader->pos[LOG_STORE_STM32], &raw[9]);
	sys_put_le64(reader->pos[LOG_STORE_ESP32], &raw[17]);
	sys_put_le32(cursor_check(raw), &raw[25]);

	for (size_t i = 0; i < CURSOR_RAW_LEN; i += 3) {
		size_t rem = MIN(3U, CURSOR_RAW_LEN - i);
		uint32_t v = (uint32_t)raw[i] << 16;

		if (rem > 1U) {
			v |= (uint32_t)raw[i + 1] << 8;
		}
		if (rem > 2U) {
			v |= raw[i + 2];
		}
		out[o++] = b64url[(v >> 18) & 0x3F];
		out[o++] = b64url[(v >> 12) & 0x3F];
		if (rem > 1U) {
			out[o++] = b64url[(v >> 6) & 0x3F];
		}
		if (rem > 2U) {
			out[o++] = b64url[v & 0x3F];
		}
	}
	out[o] = '\0';

	return (int)o;
}

int log_store_cursor_decode(const char *cursor, uint32_t boot_tag,
			    const struct log_store_filter *filter,
			    uint64_t pos[LOG_STORE_SOURCE_COUNT])
{
	uint8_t raw[CURSOR_RAW_LEN];
	size_t o = 0;
	uint64_t p[LOG_STORE_SOURCE_COUNT];
	int rc = 0;

	if (cursor == NULL || bounded_len(cursor, CURSOR_TEXT_LEN + 1U) != CURSOR_TEXT_LEN) {
		return -EINVAL;
	}

	for (size_t i = 0; i < CURSOR_TEXT_LEN; i += 4) {
		size_t rem = MIN(4U, CURSOR_TEXT_LEN - i);
		uint32_t v = 0;

		for (size_t k = 0; k < rem; k++) {
			int d = b64url_value(cursor[i + k]);

			if (d < 0) {
				return -EINVAL;
			}
			v |= (uint32_t)d << (18 - 6 * k);
		}
		raw[o++] = (uint8_t)(v >> 16);
		if (rem > 2U) {
			raw[o++] = (uint8_t)(v >> 8);
		}
		if (rem > 3U) {
			raw[o++] = (uint8_t)v;
		}
		/* Only the canonical spelling: unused low bits must be zero. */
		if ((rem == 3U && (v & 0xFFU) != 0U) || (rem == 2U && (v & 0xFFFFU) != 0U)) {
			return -EINVAL;
		}
	}

	if (o != CURSOR_RAW_LEN || raw[0] != CURSOR_VERSION ||
	    sys_get_le32(&raw[25]) != cursor_check(raw)) {
		return -EINVAL;
	}
	if (sys_get_le32(&raw[5]) != filter_digest(filter)) {
		return -EINVAL;
	}
	if (sys_get_le32(&raw[1]) != boot_tag) {
		return -ESTALE;
	}

	p[LOG_STORE_STM32] = sys_get_le64(&raw[9]);
	p[LOG_STORE_ESP32] = sys_get_le64(&raw[17]);

	(void)k_mutex_lock(&store_lock, K_FOREVER);
	for (size_t s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
		if ((p[s] & 3U) != 0U || p[s] > rings[s].head) {
			rc = -EINVAL;
		}
	}
	k_mutex_unlock(&store_lock);

	if (rc == 0) {
		memcpy(pos, p, sizeof(p));
	}
	return rc;
}

/* -- wire names ------------------------------------------------------------ */

const char *log_store_source_str(enum log_store_source source)
{
	switch (source) {
	case LOG_STORE_STM32:
		return "stm32";
	case LOG_STORE_ESP32:
		return "esp32";
	default:
		return NULL;
	}
}

const char *log_store_level_str(enum log_store_level level)
{
	switch (level) {
	case LOG_STORE_LEVEL_DEBUG:
		return "debug";
	case LOG_STORE_LEVEL_INFO:
		return "info";
	case LOG_STORE_LEVEL_WARNING:
		return "warning";
	case LOG_STORE_LEVEL_ERROR:
		return "error";
	case LOG_STORE_LEVEL_UNKNOWN:
		return "unknown";
	default:
		return NULL;
	}
}

const char *log_store_kind_str(enum log_store_kind kind)
{
	switch (kind) {
	case LOG_STORE_KIND_MESSAGE:
		return "message";
	case LOG_STORE_KIND_RESET:
		return "reset";
	case LOG_STORE_KIND_PAUSED:
		return "paused";
	case LOG_STORE_KIND_GAP:
		return "gap";
	default:
		return NULL;
	}
}
