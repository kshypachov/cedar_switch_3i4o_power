/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strict JSON request decoding for the web API.
 *
 * Contract: "Общие правила" in docs/device-development/api-contract.md -
 * UTF-8, unknown fields rejected, JSON request at most 8192 bytes - and the
 * request schemas in openapi.json.
 *
 * Why not Zephyr's JSON library. json_obj_parse() skips keys it has no
 * descriptor for (lib/utils/json.c, skip_field), and the contract requires
 * those to be refused with a pointer to the offending field. It also reports
 * one bitmask of decoded fields, not which field was wrong or why, and the
 * contract's error body names every bad field. So requests are decoded here,
 * against a static description of the schema, into a caller's struct.
 *
 * The decisions, and where they come from:
 *
 * - **Syntax and schema are different failures.** Anything that is not a
 *   single well-formed UTF-8 JSON value is 400 invalid_json and decoding stops
 *   at the first problem. A well-formed document that does not fit the schema
 *   is 422 validation_failed, and every problem found is reported, each with a
 *   JSON Pointer (RFC 6901) and one of api-validation's field codes, up to
 *   CONFIG_API_VALIDATION_MAX_FIELDS.
 *
 * - **The codes follow the mock**, because the frontend is built against the
 *   mock and must meet the same rejection on the device
 *   (tools/api-contract/cedar_contract/mock/app.py, _FIELD_CODE_BY_KEYWORD):
 *   a missing member is `required`, an undeclared one `unknown_field`, a wrong
 *   JSON type - null where null is not allowed included - `invalid_format`, a
 *   string too short `out_of_range` and too long `too_long`, an array with too
 *   few or too many elements `out_of_range`, a number outside its bounds
 *   `out_of_range`, a value outside an enum `not_allowed`.
 *
 * - **Duplicate member names make the body invalid_json**, as I-JSON
 *   (RFC 7493) requires. RFC 8259 leaves them to the implementation, and
 *   implementations disagree on whether the first or the last wins - which is
 *   how a request that one component validated means something else to the
 *   next. The mock refuses them the same way.
 *
 * - **One entry per bad value, sorted by path.** Each value gets at most one
 *   field code, chosen by precedence: too_long, out_of_range, invalid_format,
 *   not_allowed (a missing member is `required`, an undeclared one
 *   `unknown_field`, and those cannot collide with anything). Every undeclared
 *   member is reported, not only the first. Entries are sorted by JSON Pointer
 *   segment by segment, array indices numerically, so the list - and which
 *   entries survive truncation - is the same on the device and in the mock.
 *   The root is spelled "/", as the mock spells it.
 *
 * - **String lengths are counted in code points**, as JSON Schema counts
 *   minLength and maxLength, not in bytes. A password of 128 Cyrillic letters
 *   is 256 bytes and valid. Destination buffers are sized in bytes by the
 *   caller and a value that does not fit is `too_long` whatever its length in
 *   code points, so a descriptor can never overrun its struct.
 *
 * - **Integers are values, not spellings.** JSON Schema's "integer" accepts
 *   any number with a zero fractional part, so 120, 120.0 and 1.2e2 are all
 *   120, as they are to the mock's validator. Anything that is not integral,
 *   or does not fit in int64_t, is rejected.
 *
 * - **Text that C cannot hold is refused.** A string that decodes to U+0000
 *   would silently truncate a char array, and an unpaired surrogate escape has
 *   no UTF-8 spelling; both are `invalid_format` for that field. Raw bytes that
 *   are not UTF-8 make the whole body invalid_json.
 *
 * - **Bounded everything.** Nesting depth is capped (WEB_JSON_MAX_DEPTH), no
 *   allocation is made, and nothing is read past the length given.
 *
 * What this does not do: business rules (a static address needs a prefix),
 * which belong to the service the handler calls, and size limits on the body
 * itself, which web-api enforces before a byte reaches the decoder.
 */

#ifndef WEB_API_JSON_READER_H_
#define WEB_API_JSON_READER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <api_validation/api_validation.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Deepest nesting of objects and arrays a request may use. */
#define WEB_JSON_MAX_DEPTH 8

enum web_json_type {
	/** Decoded into a char array of @ref web_json_field.size bytes, NUL-terminated. */
	WEB_JSON_STRING = 0,
	/** Decoded into a bool. */
	WEB_JSON_BOOL,
	/** Decoded into an int64_t. */
	WEB_JSON_INT,
	/** Decoded in place, using @ref web_json_field.object, at the same offset. */
	WEB_JSON_OBJECT,
	/** Elements described by @ref web_json_field.items, stored in an array. */
	WEB_JSON_ARRAY,
};

/** Member must be present. */
#define WEB_JSON_REQUIRED BIT(0)
/** null is accepted and recorded in @ref web_json_field.null_offset. */
#define WEB_JSON_NULLABLE BIT(1)
/** Presence is recorded in @ref web_json_field.present_offset. */
#define WEB_JSON_PRESENT BIT(2)
/**
 * The schema's value is a `oneOf` whose branches this descriptor merges, as
 * CredentialChange and a DNS server address are in openapi.json. Any problem
 * inside the value — a wrong type, null, a missing or undeclared member, a bad
 * format, a value outside an enum — is reported as one `conflicting` entry at
 * the value itself, which is how the mock translates a oneOf no branch
 * accepts. The caller's descriptor is deliberately wider than any one branch
 * (an optional member only one branch has, an enum of every branch's values),
 * and the handler checks which branch the decoded value belongs to.
 */
#define WEB_JSON_ONEOF BIT(3)

struct web_json_object;

/**
 * One member of an object, or the element type of an array.
 *
 * Offsets are into the destination struct passed to web_json_decode() (or, for
 * array elements, into one element). Use offsetof(). Zero is a valid offset,
 * so the optional ones are used only when a flag asks for them: present_offset
 * with WEB_JSON_PRESENT, null_offset with WEB_JSON_NULLABLE. A descriptor
 * written with designated initializers therefore cannot write a flag into the
 * first member by leaving an offset out.
 */
struct web_json_field {
	/** Member name; ignored for array elements. */
	const char *name;
	enum web_json_type type;
	uint8_t flags;
	/** Where the value goes. */
	size_t offset;
	/** bool set true when the member was present (null included); WEB_JSON_PRESENT. */
	size_t present_offset;
	/** bool set true when the member was null; WEB_JSON_NULLABLE. */
	size_t null_offset;

	/** STRING: destination capacity in bytes, including the NUL. */
	uint16_t size;
	/** STRING: length bounds in code points. ARRAY: bounds on element count. */
	uint16_t min_len;
	uint16_t max_len;

	/** STRING: if non-NULL, a NULL-terminated list of the accepted values. */
	const char *const *enum_values;
	/** STRING: if non-NULL, a format check (a pattern, an address); false is
	 *  `invalid_format`. Runs only on a value that fit its buffer. */
	bool (*format)(const char *value);

	/** INT: inclusive bounds. */
	int64_t min;
	int64_t max;

	/** OBJECT: the nested description. */
	const struct web_json_object *object;

	/** ARRAY: the element description, its size, and where the count goes. */
	const struct web_json_field *items;
	size_t item_size;
	size_t count_offset;
};

/** An object: its members. Anything else present is `unknown_field`. */
struct web_json_object {
	const struct web_json_field *fields;
	size_t field_count;
};

/**
 * @brief Decode a request body into @p dest against @p schema.
 *
 * @p dest is zeroed first, so members that were absent read as zero, empty
 * strings, false and not-present. It is zeroed again on any failure, so a
 * rejected body - which may have carried a password - leaves nothing in it.
 *
 * @param body    The body bytes; need not be NUL-terminated.
 * @param len     Their number.
 * @param schema  The top-level object.
 * @param dest    Destination struct.
 * @param dest_size  sizeof(*dest), checked against every offset in debug
 *                   builds and used for the initial zeroing.
 * @param err     Filled on failure: invalid_json or validation_failed. Its
 *                request id is left empty for the caller to set.
 * @retval 0        decoded; @p err untouched
 * @retval -EBADMSG not well-formed JSON; @p err is invalid_json
 * @retval -EINVAL  well-formed but does not fit the schema; @p err is
 *                  validation_failed with fields
 */
int web_json_decode(const char *body, size_t len, const struct web_json_object *schema,
		    void *dest, size_t dest_size, struct api_error *err);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_JSON_READER_H_ */
