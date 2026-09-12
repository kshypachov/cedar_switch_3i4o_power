/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings registry implementation. Phase 1.1: the descriptor table —
 * validity checking against the README combination rules, default seeding
 * for OWNED + MIRROR_RAM keys, lookup and enumeration. Phases 1.2/1.3:
 * setting_get()/setting_set() for OWNED + MIRROR_RAM keys — scalars and
 * STRING/BYTES (caller-supplied buffer) — under the module mutex. Phase 2.1:
 * the validate hook in setting_set(); the built-in validators live in
 * settings_registry_validators.c. Phase 3.1: persistence for OWNED +
 * MIRROR_RAM + PERSISTENT keys under the "reg/" settings subtree —
 * settings_save_one() after the RAM commit in setting_set(), bulk
 * settings_load_subtree() in setting_registry_init(). Phase 3.2: OWNED +
 * MIRROR_DIRECT — no mirror at all, setting_get() reads the entry on demand
 * (settings_load_subtree_direct) and setting_set() writes straight to
 * storage, in the exact same on-flash format as MIRROR_RAM. Phase 4.1:
 * DELEGATED — get/set proxied to the owner's delegate hooks (validate still
 * runs first on set). Phase 5: subscriptions — slot pool, dedicated
 * reg_notify_wq, per-key fan-out from setting_set() with latest-value
 * coalescing (5.1); one immediate best-effort delivery of the current value
 * on subscribe, and setting_registry_notify_external_change() for values a
 * DELEGATED owner changed on its own (5.2).
 */

#include <settings_registry/settings_registry.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/toolchain.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(settings_registry, CONFIG_SETTINGS_REGISTRY_LOG_LEVEL);

/*
 * Serializes every ram_storage access, honouring the header's thread-safety
 * promise from the very first get/set. Critical sections are short and
 * I/O-free; the (blocking) persistence paths of later phases must keep their
 * flash I/O outside of it.
 */
static K_MUTEX_DEFINE(reg_lock);

/*
 * strnlen() is POSIX (only C23 adopted it), so a strict -std=c17 build
 * against the host glibc (native_sim tests) does not declare it. Open-code
 * the "length, but at most max" idiom via ISO C memchr(), which is required
 * to stop at the first match and so never reads past a shorter string's NUL.
 */
static size_t reg_strnlen(const char *s, size_t max)
{
	const char *end = memchr(s, '\0', max);

	return end != NULL ? (size_t)(end - s) : max;
}

/*
 * Single validity check shared by init, find_descriptor and foreach: a
 * descriptor that fails it is treated as never registered anywhere. The
 * table is small, so re-checking on every walk is cheaper than keeping a
 * parallel valid/invalid state. Returns NULL when valid, otherwise the
 * rejection reason for init's log.
 */
static const char *descriptor_check(const struct setting_descriptor *d)
{
	if (d->key == NULL) {
		return "NULL key";
	}

	switch (d->type) {
	case SETTING_TYPE_BOOL:
	case SETTING_TYPE_U32:
	case SETTING_TYPE_I32:
	case SETTING_TYPE_STRING:
	case SETTING_TYPE_BYTES:
		break;
#ifdef CONFIG_SETTINGS_REGISTRY_F32
	case SETTING_TYPE_F32:
		break;
#endif
	default: /* SETTING_TYPE_F32 with CONFIG_SETTINGS_REGISTRY_F32=n, or garbage */
		return "unknown value type";
	}

	if (d->storage == SETTING_STORAGE_DELEGATED) {
		if (d->persistence == SETTING_PERSISTENT) {
			return "DELEGATED must not be PERSISTENT (the owner persists it)";
		}
		if (d->delegate_get == NULL || d->delegate_set == NULL) {
			return "DELEGATED needs both delegate_get and delegate_set";
		}
		return NULL;
	}

	/* OWNED */
	if (d->mirror == SETTING_MIRROR_DIRECT) {
		if (d->persistence != SETTING_PERSISTENT) {
			return "MIRROR_DIRECT requires PERSISTENT (value has nowhere to live)";
		}
		return NULL;
	}

	if (d->ram_storage == NULL) {
		return "MIRROR_RAM without ram_storage";
	}

	return NULL;
}

static bool descriptor_is_valid(const struct setting_descriptor *d)
{
	return descriptor_check(d) == NULL;
}

/* OWNED + MIRROR_RAM only; the caller has already validated the descriptor. */
static void seed_default(const struct setting_descriptor *d)
{
	uint8_t *dst = d->ram_storage;
	size_t size;

	switch (d->type) {
	case SETTING_TYPE_BOOL:
		size = sizeof(bool);
		break;
	case SETTING_TYPE_U32:
		size = sizeof(uint32_t);
		break;
	case SETTING_TYPE_I32:
		size = sizeof(int32_t);
		break;
#ifdef CONFIG_SETTINGS_REGISTRY_F32
	case SETTING_TYPE_F32:
		size = sizeof(float);
		break;
#endif
	case SETTING_TYPE_STRING:
		if (d->max_len == 0) {
			return;
		}
		if (d->default_value == NULL) {
			memset(dst, 0, d->max_len);
		} else {
			/* bounded copy + zero fill: the default literal may be
			 * shorter than max_len and must not be read past its NUL
			 */
			size_t len = reg_strnlen(d->default_value, d->max_len - 1);

			memcpy(dst, d->default_value, len);
			memset(dst + len, 0, d->max_len - len);
		}
		return;
	case SETTING_TYPE_BYTES:
		size = d->max_len;
		break;
	default: /* unreachable: descriptor_check() rejected unknown types */
		return;
	}

	if (d->default_value == NULL) {
		memset(dst, 0, size);
	} else {
		memcpy(dst, d->default_value, size);
	}
}

/* Payload size of a scalar type, or 0 for the buffer types (STRING/BYTES). */
static size_t scalar_size(enum setting_type type)
{
	switch (type) {
	case SETTING_TYPE_BOOL:
		return sizeof(bool);
	case SETTING_TYPE_U32:
		return sizeof(uint32_t);
	case SETTING_TYPE_I32:
		return sizeof(int32_t);
#ifdef CONFIG_SETTINGS_REGISTRY_F32
	case SETTING_TYPE_F32:
		return sizeof(float);
#endif
	default:
		return 0;
	}
}

static const struct setting_descriptor *find_descriptor(const char *key)
{
	STRUCT_SECTION_FOREACH(setting_descriptor, d) {
		if (descriptor_is_valid(d) && strcmp(d->key, key) == 0) {
			return d;
		}
	}

	return NULL;
}

/* ---- Persistence (OWNED + MIRROR_RAM + PERSISTENT): "reg/" subtree ---- */

/*
 * Flash write after a successful RAM commit, called with reg_lock already
 * released — settings_save_one() blocks on flash I/O, which must never run
 * under the module mutex. No snapshot of the mirror is needed: the bytes
 * written are taken from the caller's own value, which is exactly what was
 * just committed to ram_storage, so a concurrent set of the same key cannot
 * be observed half-written here. If the flash write fails, the error is
 * returned but the mirror keeps the new value — it simply will not survive
 * the next boot. Concurrent setting_set() on the SAME key may interleave RAM
 * and flash commits in different orders (last writer wins per medium);
 * serializing writers of one key is the callers' business.
 *
 * STRING is stored WITHOUT the NUL terminator, exactly buf.len bytes — the
 * same length convention as the get/set contract; reg_set_cb() re-terminates
 * on load. BYTES is stored as the full fixed-size blob (max_len, see the
 * phase 1.3 contract). Scalars store their scalar_size() bytes.
 */
static int persist_value(const struct setting_descriptor *d, const struct setting_value *value)
{
	char path[64];
	const void *data;
	size_t len;
	int n;

	if (d->persistence != SETTING_PERSISTENT) {
		return 0;
	}

	n = snprintf(path, sizeof(path), "reg/%s", d->key);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		LOG_ERR("key \"%s\" too long for a settings path", d->key);
		return -ENAMETOOLONG;
	}

	switch (d->type) {
	case SETTING_TYPE_STRING:
		data = value->buf.data;
		len = value->buf.len;
		break;
	case SETTING_TYPE_BYTES:
		data = value->buf.data;
		len = d->max_len; /* == value->buf.len, enforced by setting_set() */
		break;
	default:
		/* union members share their address */
		data = &value->b;
		len = scalar_size(d->type);
		break;
	}

	return settings_save_one(path, data, len);
}

/*
 * Bulk-load callback: @p name arrives with the "reg/" prefix already
 * stripped, i.e. it is directly a descriptor key. Anything that cannot be
 * applied — unknown key, descriptor that is not OWNED+MIRROR_RAM+PERSISTENT,
 * size that does not fit the descriptor — is warn-and-skip, never a load
 * failure: a stale flash entry must not brick every other key's load.
 */
static int reg_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const struct setting_descriptor *d = find_descriptor(name);
	ssize_t rd;

	if (d != NULL && d->storage == SETTING_STORAGE_OWNED &&
	    d->mirror == SETTING_MIRROR_DIRECT) {
		/* Normal, not noteworthy: DIRECT entries live in the same
		 * subtree but are read on demand by setting_get() — the bulk
		 * load has nothing to seed for them. */
		return 0;
	}

	if (d == NULL || d->storage != SETTING_STORAGE_OWNED ||
	    d->mirror != SETTING_MIRROR_RAM || d->persistence != SETTING_PERSISTENT) {
		LOG_WRN("stored key \"reg/%s\" has no persistent descriptor — skipped", name);
		return 0;
	}

	switch (d->type) {
	case SETTING_TYPE_STRING:
		/* stored without NUL; the mirror needs room for one */
		if (d->max_len == 0 || len > d->max_len - 1U) {
			LOG_WRN("stored \"reg/%s\": %zu bytes exceed max_len %zu — skipped",
				name, len, d->max_len);
			return 0;
		}
		break;
	case SETTING_TYPE_BYTES:
		if (len != d->max_len) {
			LOG_WRN("stored \"reg/%s\": %zu bytes, blob is fixed at %zu — skipped",
				name, len, d->max_len);
			return 0;
		}
		break;
	default:
		if (len != scalar_size(d->type)) {
			LOG_WRN("stored \"reg/%s\": %zu bytes for a %zu-byte scalar — skipped",
				name, len, scalar_size(d->type));
			return 0;
		}
		break;
	}

	k_mutex_lock(&reg_lock, K_FOREVER);
	rd = read_cb(cb_arg, d->ram_storage, len);
	if (rd < 0 || (size_t)rd != len) {
		/* the mirror may now hold a partial read — re-seed so it
		 * stays deterministic rather than half-loaded */
		seed_default(d);
		k_mutex_unlock(&reg_lock);
		LOG_WRN("stored \"reg/%s\": read failed (%zd) — default kept", name, rd);
		return 0;
	}
	if (d->type == SETTING_TYPE_STRING) {
		/* NUL-terminate and zero the tail, same mirror invariant as
		 * seed_default()/setting_set() */
		memset((uint8_t *)d->ram_storage + len, 0, d->max_len - len);
	}
	k_mutex_unlock(&reg_lock);

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(settings_registry, "reg", NULL, reg_set_cb, NULL, NULL);

/* ---- MIRROR_DIRECT: on-demand reads (step 3.2) ---- */

/* State for one settings_load_subtree_direct() pass over an exact key. */
struct direct_read {
	uint8_t *dst;
	size_t cap;        /* how many bytes may be read into dst */
	size_t stored_len; /* on-flash size of the entry; valid when exists */
	size_t read_len;
	bool exists;
	bool overflow; /* entry present but larger than cap — nothing read */
	int err;       /* read_cb failure, if any */
};

static int direct_read_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg,
			  void *param)
{
	struct direct_read *r = param;
	ssize_t rd;

	/* Exact key requested: a non-empty residual name is a descendant,
	 * not ours (same pattern as matter-access-storage's loadDirectCb). */
	if (name != NULL && *name != '\0') {
		return 0;
	}

	r->exists = true;
	r->stored_len = len;

	if (len > r->cap) {
		r->overflow = true;
		return 1; /* stop iteration; nothing was read */
	}

	rd = read_cb(cb_arg, r->dst, len);
	if (rd < 0) {
		r->err = (int)rd;
	} else {
		r->read_len = (size_t)rd;
	}

	return 1; /* exact key handled — stop iteration */
}

/*
 * The DIRECT default comes from the descriptor, not a seeded mirror: init's
 * bulk load skips DIRECT keys entirely (there is no ram_storage to seed), so
 * a key that has never been saved is served straight from default_value —
 * through the same out contract as a real read.
 */
static int direct_get_default(const struct setting_descriptor *d, struct setting_value *out)
{
	size_t size;

	switch (d->type) {
	case SETTING_TYPE_STRING: {
		const char *src = (d->default_value != NULL) ? d->default_value : "";
		size_t len = (d->max_len == 0) ? 0 : reg_strnlen(src, d->max_len - 1);

		if (out->buf.len < len + 1U) {
			return -ENOSPC;
		}
		memcpy(out->buf.data, src, len);
		out->buf.data[len] = '\0';
		out->buf.len = len;
		return 0;
	}
	case SETTING_TYPE_BYTES:
		/* capacity >= max_len was checked before any I/O */
		if (d->default_value == NULL) {
			memset(out->buf.data, 0, d->max_len);
		} else {
			memcpy(out->buf.data, d->default_value, d->max_len);
		}
		out->buf.len = d->max_len;
		return 0;
	default:
		size = scalar_size(d->type);
		/* union members share their address */
		if (d->default_value == NULL) {
			memset(&out->b, 0, size);
		} else {
			memcpy(&out->b, d->default_value, size);
		}
		return 0;
	}
}

static int direct_get(const struct setting_descriptor *d, struct setting_value *out)
{
	char path[64];
	struct direct_read r = {0};
	int n;
	int err;

	if ((d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) &&
	    out->buf.data == NULL) {
		return -EINVAL;
	}

	/* BYTES is fixed-size — capacity can be rejected before any I/O */
	if (d->type == SETTING_TYPE_BYTES && out->buf.len < d->max_len) {
		return -ENOSPC;
	}

	n = snprintf(path, sizeof(path), "reg/%s", d->key);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		LOG_ERR("key \"%s\" too long for a settings path", d->key);
		return -ENAMETOOLONG;
	}

	switch (d->type) {
	case SETTING_TYPE_STRING:
		r.dst = out->buf.data;
		/* stored without NUL; one byte of the caller's buffer is
		 * reserved for the terminator we add */
		r.cap = (out->buf.len > 0) ? out->buf.len - 1 : 0;
		break;
	case SETTING_TYPE_BYTES:
		r.dst = out->buf.data;
		r.cap = out->buf.len;
		break;
	default:
		r.dst = (uint8_t *)&out->b; /* union members share their address */
		r.cap = scalar_size(d->type);
		break;
	}

	/* No reg_lock around this: there is no mirror to guard, and settings
	 * I/O must never run under the module mutex anyway — the settings
	 * subsystem serializes its own backend access. */
	err = settings_load_subtree_direct(path, direct_read_cb, &r);
	if (err != 0) {
		return err;
	}

	if (!r.exists) {
		return direct_get_default(d, out);
	}
	if (r.err != 0) {
		return r.err;
	}

	switch (d->type) {
	case SETTING_TYPE_STRING:
		/* a stored size this descriptor could never have written is
		 * a stale/corrupt entry — served as the default, the same
		 * warn-and-skip attitude as reg_set_cb */
		if (d->max_len == 0 || r.stored_len > d->max_len - 1U) {
			LOG_WRN("stored \"%s\": %zu bytes exceed max_len %zu — default served",
				path, r.stored_len, d->max_len);
			return direct_get_default(d, out);
		}
		if (r.overflow) {
			return -ENOSPC;
		}
		out->buf.data[r.read_len] = '\0';
		out->buf.len = r.read_len;
		return 0;
	case SETTING_TYPE_BYTES:
		if (r.stored_len != d->max_len) {
			LOG_WRN("stored \"%s\": %zu bytes, blob is fixed at %zu — default served",
				path, r.stored_len, d->max_len);
			return direct_get_default(d, out);
		}
		/* overflow impossible: capacity was pre-checked */
		out->buf.len = r.read_len;
		return 0;
	default:
		if (r.stored_len != scalar_size(d->type)) {
			LOG_WRN("stored \"%s\": %zu bytes for a %zu-byte scalar — default served",
				path, r.stored_len, scalar_size(d->type));
			return direct_get_default(d, out);
		}
		return 0;
	}
}

/* ---- Change notifications (phase 5.1) ----
 *
 * Same shape as lock_control's variant B (slot pool + dedicated work queue,
 * one k_work per subscription, coalescing on the latest payload), with two
 * differences: subscriptions are per-key (each slot pins the descriptor it
 * listens to, the fan-out filters on it) and slots are reusable
 * (setting_unsubscribe() exists, so in_use marks free slots instead of a
 * bump-only counter).
 */

struct setting_subscription {
	struct k_work work;
	const struct setting_descriptor *desc; /* which key this slot listens to */
	setting_notify_t handler;
	void *ctx;
	bool in_use;
	/* pending payload, written under reg_lock, coalesced on latest. For
	 * STRING/BYTES the bytes themselves live in pending_buf — delivery is
	 * asynchronous, the writer's buf.data is long gone by handler time. */
	struct setting_value pending;
	uint8_t pending_buf[CONFIG_SETTINGS_REGISTRY_NOTIFY_BUF_SIZE];
	/* bumped (under reg_lock) every time a real change writes pending —
	 * lets the initial-delivery snapshot detect it lost the race and
	 * must not overwrite a newer payload (step 5.2) */
	uint32_t gen;
};

static struct setting_subscription subs[CONFIG_SETTINGS_REGISTRY_MAX_SUBSCRIPTIONS];

static K_THREAD_STACK_DEFINE(reg_notify_wq_stack, CONFIG_SETTINGS_REGISTRY_NOTIFY_WORKQ_STACK_SIZE);
static struct k_work_q reg_notify_wq;
static bool reg_notify_wq_started;

static void subscription_work_handler(struct k_work *work)
{
	struct setting_subscription *slot =
		CONTAINER_OF(work, struct setting_subscription, work);
	struct setting_value snapshot;
	uint8_t local_buf[CONFIG_SETTINGS_REGISTRY_NOTIFY_BUF_SIZE];
	const char *key;
	setting_notify_t handler;
	void *ctx;

	/* Snapshot under the lock, call the handler outside it (same
	 * discipline as everywhere else: user code never runs under
	 * reg_lock). The payload is copied onto this stack because the slot
	 * buffer may be rewritten by a coalescing writer while the handler
	 * runs. */
	k_mutex_lock(&reg_lock, K_FOREVER);
	if (!slot->in_use) { /* unsubscribed between submit and execution */
		k_mutex_unlock(&reg_lock);
		return;
	}
	key = slot->desc->key;
	handler = slot->handler;
	ctx = slot->ctx;
	snapshot = slot->pending;
	if (snapshot.type == SETTING_TYPE_STRING || snapshot.type == SETTING_TYPE_BYTES) {
		memcpy(local_buf, slot->pending_buf, snapshot.buf.len);
		if (snapshot.type == SETTING_TYPE_STRING) {
			local_buf[snapshot.buf.len] = '\0'; /* room reserved below */
		}
		snapshot.buf.data = local_buf;
	}
	k_mutex_unlock(&reg_lock);

	handler(key, &snapshot, ctx);
}

/*
 * Fan a committed value out to this key's subscribers. Called only after
 * the commit fully succeeded (never for a rejected/failed write) and does
 * no flash I/O itself: it refreshes each matching slot's pending payload
 * under reg_lock and submits the slot's work item to reg_notify_wq.
 * Re-submitting before a pending delivery ran just overwrites the payload
 * (plain k_work semantics): deliveries coalesce on the latest value and a
 * slow handler never delays the writer.
 *
 * STRING/BYTES payloads are snapshotted by copy into the slot's own buffer;
 * values longer than NOTIFY_BUF_SIZE are truncated with a warning (for
 * STRING one byte is reserved so the delivered payload stays a proper
 * C string). The notification is "something changed + a snapshot" — a
 * subscriber that needs the guaranteed-full value calls setting_get().
 */
static void notify_subscribers(const struct setting_descriptor *d,
			       const struct setting_value *value)
{
	for (size_t i = 0; i < ARRAY_SIZE(subs); i++) {
		struct setting_subscription *slot = &subs[i];

		k_mutex_lock(&reg_lock, K_FOREVER);
		if (!slot->in_use || slot->desc != d) {
			k_mutex_unlock(&reg_lock);
			continue;
		}

		slot->pending = *value;
		if (d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) {
			size_t cap = (d->type == SETTING_TYPE_STRING)
					     ? sizeof(slot->pending_buf) - 1
					     : sizeof(slot->pending_buf);
			size_t len = value->buf.len;

			if (len > cap) {
				LOG_WRN("notify \"%s\": payload %zu bytes truncated to %zu",
					d->key, len, cap);
				len = cap;
			}
			memcpy(slot->pending_buf, value->buf.data, len);
			slot->pending.buf.data = slot->pending_buf;
			slot->pending.buf.len = len;
		}
		slot->gen++;
		k_mutex_unlock(&reg_lock);

		k_work_submit_to_queue(&reg_notify_wq, &slot->work);
	}
}

/* Shared tail of every successful OWNED write: persist (a no-op for
 * VOLATILE), then notify — but only when the whole commit stuck. */
static int persist_and_notify(const struct setting_descriptor *d,
			      const struct setting_value *value)
{
	int err = persist_value(d, value);

	if (err == 0) {
		notify_subscribers(d, value);
	}
	return err;
}

/*
 * Step 5.2: one immediate best-effort delivery of the key's CURRENT value,
 * scheduled right after the slot is claimed — through the same asynchronous
 * path as a real change, never synchronously in setting_subscribe().
 *
 * The snapshot is taken via the regular setting_get() outside reg_lock
 * (delegate_get and DIRECT settings I/O must not run under the module
 * mutex), then committed to the slot's pending payload under the lock — but
 * ONLY if slot->gen still equals what it was at subscribe time. A real
 * change that slipped in between bumped gen while writing its (newer)
 * payload; this older snapshot must not overwrite it, whether that delivery
 * already ran or is still queued.
 *
 * Best-effort: a DELEGATED key whose owner is not ready (delegate_get
 * fails) silently skips the initial delivery, as the header promises — the
 * subscription stays valid and future changes deliver normally. For OWNED
 * the read only fails when the value cannot fit the notify buffer
 * (-ENOSPC), which is worth a warning but is handled the same way.
 */
static void schedule_initial_delivery(struct setting_subscription *slot,
				      const struct setting_descriptor *d,
				      uint32_t gen_at_subscribe)
{
	struct setting_value v;
	uint8_t buf[CONFIG_SETTINGS_REGISTRY_NOTIFY_BUF_SIZE];
	int err;

	memset(&v, 0, sizeof(v));
	v.type = d->type;
	if (d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) {
		v.buf.data = buf;
		v.buf.len = sizeof(buf);
	}

	err = setting_get(d->key, &v);
	if (err != 0) {
		if (d->storage != SETTING_STORAGE_DELEGATED) {
			LOG_WRN("initial delivery for \"%s\" skipped (%d)", d->key, err);
		}
		return;
	}

	k_mutex_lock(&reg_lock, K_FOREVER);
	if (!slot->in_use || slot->desc != d || slot->gen != gen_at_subscribe) {
		/* a real change beat us — its newer payload wins */
		k_mutex_unlock(&reg_lock);
		return;
	}
	slot->pending = v;
	if (d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) {
		/* setting_get() bounded the read by the notify buffer size
		 * (STRING: capacity - 1 payload bytes), so the pending_buf
		 * invariants of notify_subscribers() hold here too */
		memcpy(slot->pending_buf, buf, v.buf.len);
		slot->pending.buf.data = slot->pending_buf;
		slot->pending.buf.len = v.buf.len;
	}
	k_mutex_unlock(&reg_lock);

	k_work_submit_to_queue(&reg_notify_wq, &slot->work);
}

setting_subscription_handle_t setting_subscribe(const char *key, setting_notify_t handler,
						void *ctx)
{
	const struct setting_descriptor *d;
	struct setting_subscription *slot = NULL;

	if (key == NULL || handler == NULL) {
		return NULL;
	}

	d = find_descriptor(key);
	if (d == NULL) {
		LOG_ERR("subscribe: unknown key \"%s\"", key);
		return NULL;
	}

	k_mutex_lock(&reg_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(subs); i++) {
		if (!subs[i].in_use) {
			slot = &subs[i];
			break;
		}
	}
	if (slot == NULL) {
		k_mutex_unlock(&reg_lock);
		LOG_ERR("subscription pool full (%d)",
			CONFIG_SETTINGS_REGISTRY_MAX_SUBSCRIPTIONS);
		return NULL;
	}

	slot->desc = d;
	slot->handler = handler;
	slot->ctx = ctx;
	k_work_init(&slot->work, subscription_work_handler);
	slot->in_use = true;

	uint32_t gen_at_subscribe = slot->gen;

	k_mutex_unlock(&reg_lock);

	schedule_initial_delivery(slot, d, gen_at_subscribe);

	return slot;
}

void setting_unsubscribe(setting_subscription_handle_t handle)
{
	struct setting_subscription *slot = handle;

	if (slot == NULL) {
		return;
	}

	/* Best-effort cancel outside the lock (the work handler takes
	 * reg_lock itself). A delivery already past its in_use check may
	 * still complete with the old handler — the in_use=false below stops
	 * everything after that. */
	k_work_cancel(&slot->work);

	k_mutex_lock(&reg_lock, K_FOREVER);
	slot->in_use = false;
	slot->handler = NULL;
	slot->desc = NULL;
	k_mutex_unlock(&reg_lock);
}

int setting_registry_notify_external_change(const char *key, const struct setting_value *value)
{
	const struct setting_descriptor *d;

	if (key == NULL || value == NULL) {
		return -EINVAL;
	}

	d = find_descriptor(key);
	if (d == NULL) {
		return -ENOENT;
	}

	if (d->storage != SETTING_STORAGE_DELEGATED) {
		/* OWNED has no writer other than setting_set(), which
		 * notifies by itself — see the header contract */
		return -EINVAL;
	}

	if (value->type != d->type) {
		return -EINVAL;
	}
	if ((d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) &&
	    value->buf.data == NULL) {
		return -EINVAL;
	}

	/* Post-factum by design: the value is already committed at its
	 * owner, so neither delegate_set() nor validate() runs here —
	 * calling them back would be exactly the set -> delegate_set -> set
	 * echo loop this function exists to break. */
	notify_subscribers(d, value);

	return 0;
}

int setting_get(const char *key, struct setting_value *out)
{
	const struct setting_descriptor *d;

	if (key == NULL || out == NULL) {
		return -EINVAL;
	}

	d = find_descriptor(key);
	if (d == NULL) {
		return -ENOENT;
	}

	if (out->type != d->type) {
		return -EINVAL;
	}

	if (d->storage == SETTING_STORAGE_DELEGATED) {
		/* The registry stores nothing for a DELEGATED key — the owner
		 * fills @p out itself (for STRING/BYTES: into the caller's
		 * out->buf.data within out->buf.len, per the hook contract;
		 * the registry only hands out through). No reg_lock: there is
		 * no ram_storage to guard, and foreign code must never run
		 * under the module mutex (same reason as the validate hook).
		 * The hook's return code is passed through as-is. */
		return d->delegate_get(out, d->delegate_ctx);
	}

	if (d->mirror == SETTING_MIRROR_DIRECT) {
		return direct_get(d, out);
	}

	if (d->type == SETTING_TYPE_STRING) {
		if (out->buf.data == NULL) {
			return -EINVAL;
		}

		k_mutex_lock(&reg_lock, K_FOREVER);

		/* The mirror is always NUL-terminated within max_len (both
		 * seed_default() and setting_set() guarantee it), so this is
		 * the actual length, excluding the NUL — which is what
		 * buf.len reports per the header contract.
		 */
		size_t len = reg_strnlen((const char *)d->ram_storage, d->max_len);

		/* The caller's buffer additionally gets a NUL terminator so
		 * it is directly usable as a C string; capacity must fit
		 * len + 1 even though the reported buf.len excludes the NUL.
		 */
		if (out->buf.len < len + 1U) {
			k_mutex_unlock(&reg_lock);
			return -ENOSPC;
		}

		memcpy(out->buf.data, d->ram_storage, len);
		out->buf.data[len] = '\0';
		out->buf.len = len;

		k_mutex_unlock(&reg_lock);
		return 0;
	}

	if (d->type == SETTING_TYPE_BYTES) {
		if (out->buf.data == NULL) {
			return -EINVAL;
		}

		/* Phase 1.3: a BYTES blob has no stored actual length — its
		 * size is fixed at the descriptor's max_len (setting_set()
		 * enforces exactly that many bytes on the way in). Variable
		 * length would need somewhere to keep the length, which does
		 * not exist yet — a future phase if ever needed.
		 */
		if (out->buf.len < d->max_len) {
			return -ENOSPC;
		}

		k_mutex_lock(&reg_lock, K_FOREVER);
		memcpy(out->buf.data, d->ram_storage, d->max_len);
		k_mutex_unlock(&reg_lock);

		out->buf.len = d->max_len;
		return 0;
	}

	k_mutex_lock(&reg_lock, K_FOREVER);
	switch (d->type) {
	case SETTING_TYPE_BOOL:
		out->b = *(const bool *)d->ram_storage;
		break;
	case SETTING_TYPE_U32:
		out->u32 = *(const uint32_t *)d->ram_storage;
		break;
	case SETTING_TYPE_I32:
		out->i32 = *(const int32_t *)d->ram_storage;
		break;
#ifdef CONFIG_SETTINGS_REGISTRY_F32
	case SETTING_TYPE_F32:
		out->f32 = *(const float *)d->ram_storage;
		break;
#endif
	default: /* unreachable: descriptor_check() rejected unknown types */
		break;
	}
	k_mutex_unlock(&reg_lock);

	return 0;
}

int setting_set(const char *key, const struct setting_value *value)
{
	const struct setting_descriptor *d;

	if (key == NULL || value == NULL) {
		return -EINVAL;
	}

	d = find_descriptor(key);
	if (d == NULL) {
		return -ENOENT;
	}

	if (value->type != d->type) {
		return -EINVAL;
	}

	/* Hoisted ahead of the validate hook: the hook receives the proposed
	 * value verbatim and may dereference the payload, so it must never be
	 * handed a NULL data pointer.
	 */
	if ((d->type == SETTING_TYPE_STRING || d->type == SETTING_TYPE_BYTES) &&
	    value->buf.data == NULL) {
		return -EINVAL;
	}

	/* Pre-commit hook, shared by every OWNED write path. Check order:
	 * type identity first (on a mismatch the union member the validator
	 * would read is garbage), then validate, then the storage-capacity
	 * bounds below — the domain rule sits between the structural checks;
	 * whichever refuses, nothing has been written yet. Called outside
	 * reg_lock on purpose: arbitrary user code must not run while the
	 * module mutex is held, and the hook only reads the proposed value,
	 * never ram_storage. A nonzero return is propagated as-is (no
	 * dedicated validation errno — deliberately deferred decision).
	 */
	if (d->validate != NULL) {
		int err = d->validate(value, d->validate_ctx);

		if (err != 0) {
			return err;
		}
	}

	if (d->storage == SETTING_STORAGE_DELEGATED) {
		/* Owner's write path. The type/NULL-data/validate checks above
		 * apply exactly as for OWNED — validate runs BEFORE the owner
		 * is bothered — but the max_len buffer bounds below do NOT:
		 * the registry does not store this value, so capacity limits
		 * are the owner's business inside delegate_set() (a delegated
		 * STRING legitimately carries max_len == 0). Called outside
		 * reg_lock — foreign code; its rejection code is passed
		 * through as-is, and subscribers are notified only when the
		 * owner accepted the value. */
		int err = d->delegate_set(value, d->delegate_ctx);

		if (err == 0) {
			notify_subscribers(d, value);
		}
		return err;
	}

	if (d->type == SETTING_TYPE_STRING) {
		/* buf.len excludes the NUL; the mirror must still hold the
		 * terminator, so the longest accepted string is max_len - 1.
		 */
		if (d->max_len == 0 || value->buf.len > d->max_len - 1U) {
			return -EINVAL;
		}

		/* MIRROR_DIRECT has no mirror to update — after the shared
		 * checks above, the value goes straight to settings storage
		 * via persist_value(), same on-flash format as MIRROR_RAM. */
		if (d->mirror == SETTING_MIRROR_RAM) {
			k_mutex_lock(&reg_lock, K_FOREVER);
			memcpy(d->ram_storage, value->buf.data, value->buf.len);
			/* NUL-terminate and zero the tail, consistent with
			 * seed_default() — the mirror stays fully
			 * deterministic.
			 */
			memset((uint8_t *)d->ram_storage + value->buf.len, 0,
			       d->max_len - value->buf.len);
			k_mutex_unlock(&reg_lock);
		}

		return persist_and_notify(d, value);
	}

	if (d->type == SETTING_TYPE_BYTES) {
		/* Fixed-size contract (see setting_get()): the blob is always
		 * exactly max_len bytes, so a write must provide exactly that
		 * many — there is nowhere to record a shorter actual length.
		 */
		if (value->buf.len != d->max_len) {
			return -EINVAL;
		}

		if (d->mirror == SETTING_MIRROR_RAM) {
			k_mutex_lock(&reg_lock, K_FOREVER);
			memcpy(d->ram_storage, value->buf.data, d->max_len);
			k_mutex_unlock(&reg_lock);
		}

		return persist_and_notify(d, value);
	}

	if (d->mirror == SETTING_MIRROR_RAM) {
		k_mutex_lock(&reg_lock, K_FOREVER);
		switch (d->type) {
		case SETTING_TYPE_BOOL:
			*(bool *)d->ram_storage = value->b;
			break;
		case SETTING_TYPE_U32:
			*(uint32_t *)d->ram_storage = value->u32;
			break;
		case SETTING_TYPE_I32:
			*(int32_t *)d->ram_storage = value->i32;
			break;
#ifdef CONFIG_SETTINGS_REGISTRY_F32
		case SETTING_TYPE_F32:
			*(float *)d->ram_storage = value->f32;
			break;
#endif
		default: /* unreachable: descriptor_check() rejected unknown types */
			break;
		}
		k_mutex_unlock(&reg_lock);
	}

	return persist_and_notify(d, value);
}

int setting_registry_init(void)
{
	unsigned int valid = 0;
	unsigned int rejected = 0;
	int err;

	/* Idempotent (guarded inside the settings subsystem) — another module
	 * may already have brought settings up, same ensure-pattern as
	 * reader_store.
	 */
	err = settings_subsys_init();
	if (err != 0) {
		LOG_ERR("settings subsystem init failed (%d)", err);
		return err;
	}

	/* Started once for the process lifetime — init itself is re-runnable
	 * (tests re-init to simulate reboots), a work queue thread is not. */
	if (!reg_notify_wq_started) {
		struct k_work_queue_config cfg = { .name = "reg_notify_wq" };

		k_work_queue_init(&reg_notify_wq);
		k_work_queue_start(&reg_notify_wq, reg_notify_wq_stack,
				   K_THREAD_STACK_SIZEOF(reg_notify_wq_stack),
				   CONFIG_SETTINGS_REGISTRY_NOTIFY_WORKQ_PRIORITY, &cfg);
		reg_notify_wq_started = true;
	}

	STRUCT_SECTION_FOREACH(setting_descriptor, d) {
		const char *reason = descriptor_check(d);

		if (reason != NULL) {
			LOG_ERR("descriptor \"%s\" rejected: %s",
				d->key != NULL ? d->key : "(NULL)", reason);
			rejected++;
			continue;
		}

		if (d->storage == SETTING_STORAGE_OWNED &&
		    d->mirror == SETTING_MIRROR_RAM) {
			seed_default(d);
		}
		valid++;
	}

	/* Defaults are seeded first, unconditionally; the bulk load then
	 * overwrites whichever keys actually have a persisted entry (push
	 * model via reg_set_cb). A key never saved keeps its default. No
	 * notifications are sent for loaded values (by design — subscribers
	 * learn the current value from the subscription itself, phase 5).
	 */
	err = settings_load_subtree("reg");
	if (err != 0) {
		LOG_ERR("settings load failed (%d)", err);
		return err;
	}

	LOG_INF("%u keys registered, %u rejected", valid, rejected);

	return 0;
}

void setting_registry_foreach_prefix(const char *prefix, setting_enum_cb_t cb, void *ctx)
{
	size_t plen = (prefix != NULL) ? strlen(prefix) : 0;

	STRUCT_SECTION_FOREACH(setting_descriptor, d) {
		if (!descriptor_is_valid(d)) {
			continue;
		}
		if (plen != 0 && strncmp(d->key, prefix, plen) != 0) {
			continue;
		}
		cb(d, ctx);
	}
}

void setting_registry_foreach(setting_enum_cb_t cb, void *ctx)
{
	setting_registry_foreach_prefix(NULL, cb, ctx);
}
