/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic per-key settings store for cross-module configuration (network,
 * Matter control, OTA, lock behaviour, keypad feedback, ...). Every key is
 * registered once, statically, and gets independent behaviours chosen
 * at registration time:
 *
 *  - Storage: OWNED (the registry holds the value itself) vs DELEGATED (the
 *    registry holds nothing; get/set are forwarded to the owning module's
 *    own hooks — e.g. a value that already lives as a Matter attribute).
 *  - Mirroring (OWNED only): RAM (mirrored in ram_storage; setting_get() is a
 *    plain memcpy) vs DIRECT (not mirrored at all — every setting_get() reads
 *    straight from settings storage on demand, zero permanent RAM cost. For
 *    values too large to justify a standing RAM copy, e.g. multi-kilobyte
 *    strings). Meaningless for DELEGATED, which always reads through
 *    delegate_get() regardless of this field.
 *  - Persistence (OWNED only): PERSISTENT (backed by Zephyr settings) vs
 *    VOLATILE (RAM only, reset to default on every boot — e.g. a maintenance
 *    flag that must never survive a reboot by accident). MIRROR_DIRECT
 *    requires PERSISTENT — a value that is neither mirrored nor persisted
 *    would have nowhere to be read from.
 *  - Notification: any number of subscribers can ask to be called back when
 *    a specific key changes, regardless of its storage mode.
 *
 * setting_get()/setting_set() on a STRING or BYTES value always go through a
 * caller-supplied buffer (struct setting_value.buf.data/len): on input to
 * setting_get(), len is the buffer's capacity; on return it is the actual
 * size (the call fails if the buffer is too small). This is what makes
 * MIRROR_DIRECT possible at all — there is no registry-owned copy to hand
 * out a pointer into — and it is the same contract for MIRROR_RAM, just
 * backed by a memcpy instead of an on-demand settings read.
 *
 * Persisted (OWNED + PERSISTENT) keys live under the "reg/" subtree of the
 * project's single shared Zephyr settings store (CONFIG_SETTINGS_FILE_PATH).
 * setting_registry_init() loads that whole subtree in one pass at boot, so
 * every MIRROR_RAM key's RAM mirror is populated before it returns — there
 * is no lazy/on-demand load path to warm up for those. MIRROR_DIRECT keys
 * are skipped by this bulk load entirely (nothing to seed) and cost zero RAM
 * and zero boot time no matter how large they are. CONFIG_SETTINGS_FILE_MAX_LINES
 * must stay comfortably above the total number of distinct keys sharing that
 * one file (this registry's keys plus every other module already using
 * settings), or every write degrades into a full-file rewrite.
 *
 * Registration is static and link-time only (SETTING_REGISTRY_DEFINE, backed
 * by Zephyr's iterable sections) — a key cannot be added or removed at
 * runtime, only subscribed to. This is what makes "add a new setting" a
 * change local to the owning module's own source file, with nothing central
 * to edit.
 *
 * Echo avoidance for DELEGATED keys: setting_set() calls the owner's
 * delegate_set() and then notifies subscribers. If the owner's value changes
 * on its own (e.g. a Matter controller writes the attribute directly,
 * bypassing this registry), the owner must call
 * setting_registry_notify_external_change() itself to fan that out —
 * calling setting_set() again from inside delegate_set() would loop.
 *
 * Thread safety: setting_get()/setting_set()/subscribe/unsubscribe are safe
 * from any thread. setting_set() on a PERSISTENT key blocks the caller for
 * the flash write, same as the existing reader_store/lib_wifi settings
 * usage — do not call it from a context that cannot block. Notification
 * delivery is asynchronous (dedicated work queue, one work item per
 * subscription) and never blocks the writer; consecutive changes to the same
 * key before a subscriber's handler has run coalesce, so the handler always
 * sees the latest value rather than every intermediate one.
 *
 * Subscribe-before-set is a supported, ordinary case, not a race: a task
 * never has to know whether the key it cares about already has a real value
 * yet, or whether it will be some other task that eventually sets it.
 * setting_subscribe() always schedules one immediate best-effort delivery of
 * the current value through the same asynchronous path as a real change, so
 * "subscribed early" and "subscribed after the value existed" behave the
 * same way from the subscriber's point of view. What cannot happen at
 * runtime is a key appearing that no descriptor was ever linked in for —
 * the set of valid keys is fixed at build time (see registration, below).
 */

#pragma once

#include <zephyr/sys/iterable_sections.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Value model ---- */

enum setting_type {
	SETTING_TYPE_BOOL,
	SETTING_TYPE_U32,
	SETTING_TYPE_I32,
	SETTING_TYPE_F32,    /* requires CONFIG_SETTINGS_REGISTRY_F32=y — see struct setting_value;
			      * the tag itself always exists so the other tags' numeric values
			      * stay stable across configs, but a descriptor using it must be
			      * rejected at registration/init when the option is off, same as
			      * an unknown type would be. Until that option is on, treat this
			      * type as if the registry did not know about it at all. */
	SETTING_TYPE_STRING, /* bounded, NUL-terminated, max_len from the descriptor */
	SETTING_TYPE_BYTES,  /* opaque blob, max_len from the descriptor */
};

struct setting_value {
	enum setting_type type;
	union {
		bool b;
		uint32_t u32;
		int32_t i32;
#ifdef CONFIG_SETTINGS_REGISTRY_F32
		float f32;
#endif
		struct {
			uint8_t *data;
			size_t len;
		} buf; /* STRING (len excludes the NUL) or BYTES */
	};
};

/* ---- Per-key behaviour, fixed at registration ---- */

enum setting_storage {
	SETTING_STORAGE_OWNED,     /* registry holds the RAM mirror (+ optional persist) */
	SETTING_STORAGE_DELEGATED, /* registry holds nothing; forwards to the owner's hooks */
};

enum setting_persistence {
	SETTING_VOLATILE,   /* RAM only. Valid for OWNED; the only allowed value for DELEGATED */
	SETTING_PERSISTENT, /* settings_save_one()/settings_load_subtree(), OWNED only */
};

enum setting_mirror {
	SETTING_MIRROR_RAM,    /* value lives in ram_storage; setting_get() is a memcpy, no flash I/O.
				 * Meaningful for OWNED only — ignored for DELEGATED. */
	SETTING_MIRROR_DIRECT, /* not mirrored; every setting_get()/setting_set() reads/writes
				 * settings storage directly on demand (settings_load_subtree_direct()/
				 * settings_save_one()). No ram_storage needed — zero RAM cost
				 * regardless of value size. Requires SETTING_PERSISTENT (see
				 * setting_persistence); skipped entirely by setting_registry_init()'s
				 * bulk load. Intended for large STRING/BYTES values read rarely
				 * enough that a standing RAM mirror would not be worth its size. */
};

/**
 * DELEGATED get/set hooks, implemented by the owning module.
 *
 * @return 0 on success. delegate_set() may return a negative errno to reject
 *         the new value (e.g. -EINVAL) — on rejection the registry does not
 *         notify subscribers.
 */
typedef int (*setting_delegate_get_t)(struct setting_value *out, void *ctx);
typedef int (*setting_delegate_set_t)(const struct setting_value *in, void *ctx);

/**
 * Optional pre-commit check, called for both storage modes before a new
 * value is written (RAM and/or delegate_set()).
 *
 * @return 0 to accept, negative errno to reject — nothing is written and no
 *         subscriber is notified.
 */
typedef int (*setting_validate_t)(const struct setting_value *proposed, void *ctx);

/**
 * Change notification, fired after a value has been committed (never fired
 * if validate() or delegate_set() rejected it). @p key is the full
 * descriptor key the change belongs to — the same handler may be subscribed
 * to more than one key, @p ctx is whatever was passed to setting_subscribe().
 */
typedef void (*setting_notify_t)(const char *key, const struct setting_value *value, void *ctx);

/* ---- Descriptor: one per key, registered statically ---- */

struct setting_descriptor {
	const char *key; /* e.g. "net/wifi/ssid", "lock/auto_relock/timeout_s" */
	enum setting_type type;
	enum setting_storage storage;
	enum setting_persistence persistence; /* must be SETTING_VOLATILE when storage is DELEGATED */
	enum setting_mirror mirror; /* OWNED only, ignored for DELEGATED; SETTING_MIRROR_DIRECT
				      * requires persistence == SETTING_PERSISTENT */
	size_t max_len;             /* STRING/BYTES only; ignored for scalar types */
	const void *default_value;  /* seeds ram_storage at init; used verbatim if never persisted.
				      * MIRROR_DIRECT: still used to seed storage the first time this
				      * key is ever written, same as MIRROR_RAM. */

	void *ram_storage; /* OWNED + SETTING_MIRROR_RAM only: static backing buffer, sized max_len
			     * (or the scalar's sizeof); registry-private storage — do not read/write
			     * it directly, always go through setting_get()/setting_set(). Unused for
			     * DELEGATED and for OWNED + SETTING_MIRROR_DIRECT (leave NULL there —
			     * nothing is ever mirrored in RAM for that key). */

	setting_validate_t validate; /* optional, both storage modes */
	void *validate_ctx;

	setting_delegate_get_t delegate_get; /* DELEGATED only */
	setting_delegate_set_t delegate_set; /* DELEGATED only */
	void *delegate_ctx;
};

/**
 * Define one setting descriptor, collected at link time into the registry's
 * iterable section — no central list to edit when adding a new key. Place
 * next to the owning module's own code.
 *
 * Example (OWNED, persistent, RAM-mirrored — the common case):
 *   static char ssid_storage[33];
 *   SETTING_REGISTRY_DEFINE(wifi_ssid, .key = "net/wifi/ssid",
 *                            .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
 *                            .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
 *                            .max_len = sizeof(ssid_storage),
 *                            .default_value = "", .ram_storage = ssid_storage);
 *
 * Example (OWNED, persistent, NOT RAM-mirrored — for a value too large to keep
 * standing in RAM, e.g. a multi-kilobyte certificate blob; no ram_storage at all):
 *   SETTING_REGISTRY_DEFINE(ota_changelog, .key = "ota/changelog",
 *                            .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
 *                            .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_DIRECT,
 *                            .max_len = 4096, .default_value = "");
 *
 * Example (DELEGATED):
 *   SETTING_REGISTRY_DEFINE(auto_relock, .key = "lock/auto_relock/timeout_s",
 *                            .type = SETTING_TYPE_U32, .storage = SETTING_STORAGE_DELEGATED,
 *                            .persistence = SETTING_VOLATILE,
 *                            .delegate_get = matter_get_auto_relock,
 *                            .delegate_set = matter_set_auto_relock,
 *                            .validate = setting_validate_range_u32,
 *                            .validate_ctx = &(struct setting_range_u32){ .min = 0, .max = 600 });
 *   (the compound literal above is fine: it is written at file scope, same as the descriptor
 *   itself, so it gets static storage duration automatically — no separate named variable
 *   needed just to hold two constraint numbers.)
 */
#define SETTING_REGISTRY_DEFINE(_name, ...) \
	static STRUCT_SECTION_ITERABLE(setting_descriptor, _name) = { __VA_ARGS__ }

/* ---- Built-in validators ----
 *
 * Common, ready-made setting_validate_t implementations so most keys never
 * need a hand-written validate function — only genuinely key-specific rules
 * do. Each expects validate_ctx to point at the matching struct below
 * (typically a file-scope compound literal right in the SETTING_REGISTRY_DEFINE
 * call, see the DELEGATED example above).
 */

struct setting_range_u32 {
	uint32_t min;
	uint32_t max;
}; /* inclusive */

struct setting_range_i32 {
	int32_t min;
	int32_t max;
}; /* inclusive */

#ifdef CONFIG_SETTINGS_REGISTRY_F32
struct setting_range_f32 {
	float min;
	float max;
}; /* inclusive */
#endif

struct setting_string_len {
	size_t min_len;
	size_t max_len; /* further restricts the descriptor's own max_len, does not replace it */
};

struct setting_u32_oneof {
	const uint32_t *allowed;
	size_t count;
};

int setting_validate_range_u32(const struct setting_value *proposed, void *ctx);
int setting_validate_range_i32(const struct setting_value *proposed, void *ctx);
#ifdef CONFIG_SETTINGS_REGISTRY_F32
int setting_validate_range_f32(const struct setting_value *proposed, void *ctx);
#endif
int setting_validate_string_len(const struct setting_value *proposed, void *ctx);
int setting_validate_u32_oneof(const struct setting_value *proposed, void *ctx);

/* ---- Lifecycle ---- */

/**
 * Mount the "reg/" settings subtree and load it in one pass: every
 * SETTING_MIRROR_RAM descriptor's RAM mirror is seeded with its
 * default_value first, then overwritten for whichever keys actually have a
 * persisted entry. SETTING_MIRROR_DIRECT keys are not touched by this pass
 * at all — there is nothing to seed, their storage is read fresh on every
 * setting_get() instead. Must run after the descriptors it covers are linked
 * in (i.e. not before any translation unit's SETTING_REGISTRY_DEFINE has
 * been processed) and before the first setting_get()/setting_set() call.
 *
 * @return 0 on success, negative errno on a settings subsystem failure.
 */
int setting_registry_init(void);

/* ---- Per-key access ---- */

/**
 * Read the current value: a memcpy from the RAM mirror for
 * SETTING_MIRROR_RAM keys, an on-demand settings read for
 * SETTING_MIRROR_DIRECT keys, or a call into delegate_get() for DELEGATED
 * keys.
 *
 * For SETTING_TYPE_STRING/SETTING_TYPE_BYTES, @p out->buf.data/len must be
 * set by the caller before the call: data points at a caller-owned buffer,
 * len is its capacity. On success len is updated to the actual size. This is
 * the only way SETTING_MIRROR_DIRECT can work at all (there is no
 * registry-owned copy to hand out a pointer into) and applies uniformly to
 * SETTING_MIRROR_RAM too, so callers do not need to know which mode a key
 * uses.
 *
 * @return 0 on success, -ENOENT if @p key is not registered, -ENOSPC if the
 *         caller's buffer (STRING/BYTES) is smaller than the stored value,
 *         negative errno propagated from delegate_get() otherwise.
 */
int setting_get(const char *key, struct setting_value *out);

/**
 * Write a new value: validate() (if set) -> update RAM and/or persist
 * (OWNED) or call delegate_set() (DELEGATED) -> notify subscribers.
 * Nothing downstream of validate()/delegate_set() runs if either rejects
 * the value. For SETTING_MIRROR_DIRECT keys there is no RAM mirror to
 * update — the new value goes straight to settings storage (still subject
 * to the same blocking-caller/thread-safety rules as any PERSISTENT key).
 *
 * @return 0 on success, -ENOENT if @p key is not registered, negative errno
 *         from validate()/delegate_set() on rejection.
 */
int setting_set(const char *key, const struct setting_value *value);

/**
 * DELEGATED-only: the owner calls this after changing its value through some
 * path other than setting_set() (e.g. a Matter controller wrote the
 * attribute directly), so subscribers still get notified. Does not call
 * delegate_set() or validate() — the value is already committed at the
 * source. Rejected for OWNED keys, which have no writer other than
 * setting_set().
 *
 * @return 0 on success, -ENOENT if @p key is not registered, -EINVAL if
 *         @p key is OWNED.
 */
int setting_registry_notify_external_change(const char *key, const struct setting_value *value);

/* ---- Change notification ---- */

struct setting_subscription;
typedef struct setting_subscription *setting_subscription_handle_t;

/**
 * Subscribe to changes of one key. Delivery is asynchronous (dedicated work
 * queue) and never blocks setting_set()/setting_registry_notify_external_change();
 * see the file header for coalescing semantics.
 *
 * Subscribing also schedules one immediate, best-effort delivery of the
 * key's CURRENT value, the same way a real change would be delivered — so a
 * subscriber never has to care whether it registered before or after the
 * value was first set (by settings load at boot, by another task's
 * setting_set(), or by setting_registry_notify_external_change()). For a
 * DELEGATED key whose owner is not ready yet (delegate_get() fails), this
 * initial delivery is silently skipped; the subscriber then simply waits
 * for the owner's first real change.
 *
 * @return a handle to pass to setting_unsubscribe(), or NULL if @p key is
 *         unknown, @p handler is NULL, or the subscription table is full
 *         (CONFIG_SETTINGS_REGISTRY_MAX_SUBSCRIPTIONS).
 */
setting_subscription_handle_t setting_subscribe(const char *key, setting_notify_t handler, void *ctx);

/** Remove a subscription. Safe to call with NULL (no-op). */
void setting_unsubscribe(setting_subscription_handle_t handle);

/* ---- Enumeration (for a dynamic REST/WS surface over every registered key) ---- */

typedef void (*setting_enum_cb_t)(const struct setting_descriptor *desc, void *ctx);

/** Invoke @p cb once per registered descriptor, in registration order. */
void setting_registry_foreach(setting_enum_cb_t cb, void *ctx);

/**
 * Like setting_registry_foreach(), but @p cb runs only for descriptors whose
 * key starts with @p prefix (e.g. "net/" or "lock/"), in the same
 * registration order. A NULL or empty @p prefix matches every key, making
 * this equivalent to setting_registry_foreach().
 */
void setting_registry_foreach_prefix(const char *prefix, setting_enum_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
