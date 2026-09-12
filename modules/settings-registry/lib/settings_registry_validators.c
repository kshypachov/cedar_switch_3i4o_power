/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Built-in validators (phase 2.1) — the ready-made setting_validate_t
 * implementations declared in settings_registry.h, kept separate from the
 * registry core: they never touch descriptors, ram_storage or reg_lock,
 * only the proposed value and their own ctx struct.
 *
 * Shared conventions: a NULL proposed value, a NULL/incomplete ctx or a
 * proposed type other than what the validator understands is a registration
 * bug, not a runtime condition — rejected with the same -EINVAL as an
 * out-of-range value (a dedicated validation errno is a deliberately
 * deferred decision). Return 0 to accept, -EINVAL to reject.
 */

#include <settings_registry/settings_registry.h>

#include <errno.h>
#include <stddef.h>

int setting_validate_range_u32(const struct setting_value *proposed, void *ctx)
{
	const struct setting_range_u32 *range = (const struct setting_range_u32 *)ctx;

	if (proposed == NULL || range == NULL || proposed->type != SETTING_TYPE_U32) {
		return -EINVAL;
	}

	return (proposed->u32 >= range->min && proposed->u32 <= range->max) ? 0 : -EINVAL;
}

int setting_validate_range_i32(const struct setting_value *proposed, void *ctx)
{
	const struct setting_range_i32 *range = (const struct setting_range_i32 *)ctx;

	if (proposed == NULL || range == NULL || proposed->type != SETTING_TYPE_I32) {
		return -EINVAL;
	}

	return (proposed->i32 >= range->min && proposed->i32 <= range->max) ? 0 : -EINVAL;
}

#ifdef CONFIG_SETTINGS_REGISTRY_F32
int setting_validate_range_f32(const struct setting_value *proposed, void *ctx)
{
	const struct setting_range_f32 *range = (const struct setting_range_f32 *)ctx;

	if (proposed == NULL || range == NULL || proposed->type != SETTING_TYPE_F32) {
		return -EINVAL;
	}

	return (proposed->f32 >= range->min && proposed->f32 <= range->max) ? 0 : -EINVAL;
}
#endif /* CONFIG_SETTINGS_REGISTRY_F32 */

int setting_validate_string_len(const struct setting_value *proposed, void *ctx)
{
	const struct setting_string_len *len = (const struct setting_string_len *)ctx;

	if (proposed == NULL || len == NULL || proposed->type != SETTING_TYPE_STRING) {
		return -EINVAL;
	}

	/* buf.len excludes the NUL, same as everywhere in the get/set contract */
	return (proposed->buf.len >= len->min_len && proposed->buf.len <= len->max_len)
		       ? 0
		       : -EINVAL;
}

int setting_validate_u32_oneof(const struct setting_value *proposed, void *ctx)
{
	const struct setting_u32_oneof *oneof = (const struct setting_u32_oneof *)ctx;

	if (proposed == NULL || oneof == NULL || oneof->allowed == NULL ||
	    proposed->type != SETTING_TYPE_U32) {
		return -EINVAL;
	}

	for (size_t i = 0; i < oneof->count; i++) {
		if (oneof->allowed[i] == proposed->u32) {
			return 0;
		}
	}

	return -EINVAL;
}
