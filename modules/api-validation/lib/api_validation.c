/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validation and error codes for the web API. See
 * include/api_validation/api_validation.h for the contract; this file
 * documents only how it is achieved.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <api_validation/api_validation.h>

/*
 * The contract's error table, transcribed once. Keeping the wire name, the
 * status and the retryable flag in a single row is the point: they are three
 * facets of one decision, and splitting them across three switch statements is
 * how they drift apart.
 */
struct error_entry {
	const char *name;
	uint16_t status;
	bool retryable;
};

static const struct error_entry error_table[API_ERR_COUNT] = {
	[API_ERR_INVALID_JSON] = {"invalid_json", 400, false},
	[API_ERR_INVALID_QUERY] = {"invalid_query", 400, false},
	[API_ERR_INVALID_CURSOR] = {"invalid_cursor", 400, false},

	[API_ERR_AUTHENTICATION_REQUIRED] = {"authentication_required", 401, false},
	[API_ERR_INVALID_CREDENTIALS] = {"invalid_credentials", 401, false},
	/* Retryable: logging in again is exactly what the client should do. */
	[API_ERR_SESSION_EXPIRED] = {"session_expired", 401, true},

	[API_ERR_CSRF_FAILED] = {"csrf_failed", 403, false},
	[API_ERR_ORIGIN_REJECTED] = {"origin_rejected", 403, false},
	[API_ERR_SETUP_NOT_ALLOWED] = {"setup_not_allowed", 403, false},

	[API_ERR_NOT_FOUND] = {"not_found", 404, false},

	/* The 409s divide cleanly: a conflict of timing is retryable, a
	 * conflict of content is not. Repeating a request while another
	 * operation finishes can succeed; repeating one built on a revision
	 * that has moved on cannot, and the client must re-read first.
	 */
	[API_ERR_BUSY] = {"busy", 409, true},
	[API_ERR_STALE_REVISION] = {"stale_revision", 409, false},
	[API_ERR_INVALID_STATE] = {"invalid_state", 409, false},
	[API_ERR_OFFSET_MISMATCH] = {"offset_mismatch", 409, false},
	[API_ERR_IDEMPOTENCY_CONFLICT] = {"idempotency_conflict", 409, false},
	[API_ERR_ETHERNET_REQUIRED] = {"ethernet_required", 409, false},

	[API_ERR_RESOURCE_EXPIRED] = {"resource_expired", 410, false},
	[API_ERR_BOOT_CHANGED] = {"boot_changed", 410, false},

	[API_ERR_PAYLOAD_TOO_LARGE] = {"payload_too_large", 413, false},
	[API_ERR_UNSUPPORTED_MEDIA_TYPE] = {"unsupported_media_type", 415, false},

	[API_ERR_VALIDATION_FAILED] = {"validation_failed", 422, false},
	[API_ERR_INVALID_IMAGE] = {"invalid_image", 422, false},
	[API_ERR_UNSUPPORTED_TARGET] = {"unsupported_target", 422, false},
	[API_ERR_INCOMPATIBLE_FIRMWARE] = {"incompatible_firmware", 422, false},
	[API_ERR_SIGNATURE_INVALID] = {"signature_invalid", 422, false},

	[API_ERR_RATE_LIMITED] = {"rate_limited", 429, true},

	/*
	 * Retryable: an internal error is by definition one we did not predict,
	 * and the honest answer is that trying again might work. It carries no
	 * detail beyond the code, per the contract's rule against stack traces.
	 */
	[API_ERR_INTERNAL_ERROR] = {"internal_error", 500, true},

	[API_ERR_SERVICE_NOT_READY] = {"service_not_ready", 503, true},
	/*
	 * Not retryable, unlike its neighbour: the contract uses this for a
	 * capability the build does not have — ESP32 OTA in this version — and
	 * no amount of waiting adds it.
	 */
	[API_ERR_CAPABILITY_UNAVAILABLE] = {"capability_unavailable", 503, false},

	[API_ERR_STORAGE_FULL] = {"storage_full", 507, false},
};

static const char *const field_code_names[API_FIELD_CODE_COUNT] = {
	[API_FIELD_REQUIRED] = "required",
	[API_FIELD_INVALID_FORMAT] = "invalid_format",
	[API_FIELD_OUT_OF_RANGE] = "out_of_range",
	[API_FIELD_TOO_LONG] = "too_long",
	[API_FIELD_UNKNOWN] = "unknown_field",
	[API_FIELD_CONFLICTING] = "conflicting",
	[API_FIELD_NOT_ALLOWED] = "not_allowed",
};

static bool code_in_range(enum api_error_code code)
{
	return (int)code >= 0 && (int)code < API_ERR_COUNT;
}

uint16_t api_error_status(enum api_error_code code)
{
	/*
	 * An unknown code is a programming error, and answering 500 is the
	 * only defensible response: it is the one status that promises nothing
	 * to the client about what went wrong.
	 */
	return code_in_range(code) ? error_table[code].status : 500U;
}

const char *api_error_str(enum api_error_code code)
{
	return code_in_range(code) ? error_table[code].name : NULL;
}

bool api_error_is_retryable(enum api_error_code code)
{
	return code_in_range(code) ? error_table[code].retryable : false;
}

const char *api_field_code_str(enum api_field_code code)
{
	if ((int)code < 0 || (int)code >= API_FIELD_CODE_COUNT) {
		return NULL;
	}

	return field_code_names[code];
}

/* --- request ids -------------------------------------------------------- */

static uint32_t request_id_state;
static bool request_id_seeded;

void api_request_id_seed(uint32_t seed)
{
	request_id_state = seed;
	request_id_seeded = true;
}

int api_request_id_generate(char *buf, size_t cap)
{
	/* "req_" + 8 hex digits + NUL. */
	if (buf == NULL || cap < 13U) {
		return -ENOMEM;
	}

	if (!request_id_seeded) {
		/*
		 * Seeded once from the cycle counter, so two boots do not hand
		 * out the same ids and a log from one is not confused with a
		 * log from the other. Deliberately not the random subsystem: a
		 * request id is a correlation handle that is never accepted as
		 * input anywhere, so it does not need to be unguessable, and
		 * an entropy dependency in a module with no other I/O would be
		 * paid for nothing.
		 */
		api_request_id_seed(k_cycle_get_32());
	}

	request_id_state++;
	(void)snprintf(buf, cap, "req_%08x", request_id_state);

	return 0;
}

/* --- errors ------------------------------------------------------------- */

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
	size_t i = 0;

	if (src != NULL) {
		while (i + 1U < dst_size && src[i] != '\0') {
			dst[i] = src[i];
			i++;
		}
	}
	dst[i] = '\0';
}

int api_error_init(struct api_error *err, enum api_error_code code, const char *message,
		   const char *request_id)
{
	if (err == NULL || !code_in_range(code)) {
		return -EINVAL;
	}
	if (request_id != NULL && !api_validate_opaque_id(request_id)) {
		return -EINVAL;
	}

	memset(err, 0, sizeof(*err));
	err->code = code;
	copy_bounded(err->message, sizeof(err->message), message);

	if (request_id != NULL) {
		copy_bounded(err->request_id, sizeof(err->request_id), request_id);
	} else if (api_request_id_generate(err->request_id, sizeof(err->request_id)) != 0) {
		return -EINVAL;
	}

	return 0;
}

/* A JSON Pointer, which is what the contract's example uses for field paths. */
static bool path_is_valid(const char *path)
{
	if (path == NULL || path[0] != '/') {
		return false;
	}

	size_t i = 0;

	while (path[i] != '\0') {
		/*
		 * Control characters would have to be escaped as \u00xx, and a
		 * path containing one is a bug in the caller rather than
		 * something to render faithfully.
		 */
		if ((unsigned char)path[i] < 0x20U) {
			return false;
		}
		i++;
		if (i > API_ERROR_PATH_MAX_LEN) {
			return false;
		}
	}

	return true;
}

int api_error_add_field(struct api_error *err, const char *path, enum api_field_code code)
{
	if (err == NULL || !path_is_valid(path) || api_field_code_str(code) == NULL) {
		return -EINVAL;
	}

	if (err->field_count >= ARRAY_SIZE(err->fields)) {
		/*
		 * Recorded rather than hidden. A client shown three of seven
		 * bad fields fixes three and is rejected again, with no way to
		 * know more were waiting.
		 */
		err->fields_truncated = true;
		return -ENOSPC;
	}

	struct api_error_field *f = &err->fields[err->field_count];

	copy_bounded(f->path, sizeof(f->path), path);
	f->code = code;
	err->field_count++;

	return 0;
}

int api_error_set_retry_after(struct api_error *err, uint16_t seconds)
{
	if (err == NULL) {
		return -EINVAL;
	}

	err->retry_after_seconds = seconds;

	return 0;
}

/*
 * Minimal JSON string escaping: the two characters that would end the string
 * or the escape, plus control characters as \u00xx. Everything else, UTF-8
 * included, passes through — the contract says bodies are UTF-8, and escaping
 * non-ASCII would only make them harder to read.
 */
static int append_escaped(char *buf, size_t cap, size_t pos, const char *src)
{
	for (size_t i = 0; src[i] != '\0'; i++) {
		char c = src[i];
		const char *esc = NULL;
		char unicode[7];

		switch (c) {
		case '"':
			esc = "\\\"";
			break;
		case '\\':
			esc = "\\\\";
			break;
		case '\n':
			esc = "\\n";
			break;
		case '\r':
			esc = "\\r";
			break;
		case '\t':
			esc = "\\t";
			break;
		default:
			if ((unsigned char)c < 0x20U) {
				(void)snprintf(unicode, sizeof(unicode), "\\u%04x",
					       (unsigned int)(unsigned char)c);
				esc = unicode;
			}
			break;
		}

		if (esc != NULL) {
			size_t n = strlen(esc);

			if (pos + n >= cap) {
				return -ENOMEM;
			}
			memcpy(&buf[pos], esc, n);
			pos += n;
		} else {
			if (pos + 1U >= cap) {
				return -ENOMEM;
			}
			buf[pos++] = c;
		}
	}

	return (int)pos;
}

static int append_literal(char *buf, size_t cap, size_t pos, const char *src)
{
	size_t n = strlen(src);

	if (pos + n >= cap) {
		return -ENOMEM;
	}
	memcpy(&buf[pos], src, n);

	return (int)(pos + n);
}

/* Chains an append, collapsing the error check that would otherwise repeat. */
#define APPEND(fn, arg)                                                                            \
	do {                                                                                       \
		int _r = fn(buf, cap, pos, arg);                                                    \
		if (_r < 0) {                                                                       \
			return -ENOMEM;                                                            \
		}                                                                                  \
		pos = (size_t)_r;                                                                  \
	} while (0)

int api_error_to_json(const struct api_error *err, char *buf, size_t cap)
{
	if (err == NULL || buf == NULL || cap == 0U) {
		return -ENOMEM;
	}
	if (!code_in_range(err->code)) {
		return -ENOMEM;
	}

	size_t pos = 0;

	APPEND(append_literal, "{\"error\":{\"code\":\"");
	APPEND(append_literal, error_table[err->code].name);
	APPEND(append_literal, "\",\"message\":\"");
	APPEND(append_escaped, err->message);
	if (err->fields_truncated) {
		/*
		 * The only place this fits. ErrorDetail is additionalProperties
		 * false, so a truncation marker cannot be its own field without
		 * a contract change, and a client shown three of seven bad
		 * fields would otherwise fix three and be rejected again with
		 * no hint that more were waiting.
		 */
		APPEND(append_literal, " (further problems were not reported)");
	}
	APPEND(append_literal, "\",\"request_id\":\"");
	APPEND(append_escaped, err->request_id);
	APPEND(append_literal, "\",\"retryable\":");
	APPEND(append_literal, error_table[err->code].retryable ? "true" : "false");

	/*
	 * `fields` is optional in the schema, so it is omitted rather than sent
	 * empty. An empty array reads as "we checked the fields and they are
	 * fine", which is not what a rejection with no field detail means.
	 */
	if (err->field_count > 0U) {
		APPEND(append_literal, ",\"fields\":[");
		for (uint8_t i = 0; i < err->field_count; i++) {
			if (i > 0U) {
				APPEND(append_literal, ",");
			}
			APPEND(append_literal, "{\"path\":\"");
			APPEND(append_escaped, err->fields[i].path);
			APPEND(append_literal, "\",\"code\":\"");
			APPEND(append_literal, field_code_names[err->fields[i].code]);
			APPEND(append_literal, "\"}");
		}
		APPEND(append_literal, "]");
	}

	APPEND(append_literal, "}}");

	buf[pos] = '\0';

	return (int)pos;
}

#undef APPEND

/* --- validators --------------------------------------------------------- */

bool api_validate_idempotency_key(const char *key)
{
	if (key == NULL) {
		return false;
	}

	size_t len = 0;

	while (len <= API_IDEMPOTENCY_KEY_MAX_LEN && key[len] != '\0') {
		unsigned char c = (unsigned char)key[len];

		/* Printable ASCII: the value is logged and used as a map key. */
		if (c < 0x21U || c > 0x7EU) {
			return false;
		}
		len++;
	}

	return len >= API_IDEMPOTENCY_KEY_MIN_LEN && len <= API_IDEMPOTENCY_KEY_MAX_LEN;
}

bool api_validate_opaque_id(const char *id)
{
	if (id == NULL) {
		return false;
	}

	size_t len = 0;

	while (len <= API_REQUEST_ID_MAX_LEN && id[len] != '\0') {
		char c = id[len];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-')) {
			return false;
		}
		len++;
	}

	return len > 0U && len <= API_REQUEST_ID_MAX_LEN;
}

/*
 * One decimal octet. Returns the number of characters consumed, or -EINVAL.
 * Leading zeros are rejected rather than skipped: "010" means ten to this
 * parser and eight to some others, and an address whose meaning depends on
 * which library read it is not an address anyone configured on purpose.
 */
static int parse_octet(const char *s, uint8_t *out)
{
	if (s[0] < '0' || s[0] > '9') {
		return -EINVAL;
	}

	int len = 0;
	unsigned int value = 0;

	while (s[len] >= '0' && s[len] <= '9') {
		if (len == 3) {
			return -EINVAL;
		}
		value = value * 10U + (unsigned int)(s[len] - '0');
		len++;
	}

	if (len > 1 && s[0] == '0') {
		return -EINVAL;
	}
	if (value > 255U) {
		return -EINVAL;
	}

	*out = (uint8_t)value;

	return len;
}

int api_parse_ipv4(const char *text, uint8_t out[4])
{
	if (text == NULL || out == NULL) {
		return -EINVAL;
	}

	uint8_t octets[4];
	size_t pos = 0;

	for (int i = 0; i < 4; i++) {
		if (i > 0) {
			if (text[pos] != '.') {
				return -EINVAL;
			}
			pos++;
		}

		int n = parse_octet(&text[pos], &octets[i]);

		if (n < 0) {
			return -EINVAL;
		}
		pos += (size_t)n;
	}

	/* Nothing trailing, not even whitespace. */
	if (text[pos] != '\0') {
		return -EINVAL;
	}

	memcpy(out, octets, sizeof(octets));

	return 0;
}

/* One hex group of an IPv6 address: 1-4 hex digits. */
static int parse_hex_group(const char *s, uint16_t *out)
{
	int len = 0;
	unsigned int value = 0;

	while (len < 4) {
		char c = s[len];
		unsigned int digit;

		if (c >= '0' && c <= '9') {
			digit = (unsigned int)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			digit = (unsigned int)(c - 'a') + 10U;
		} else if (c >= 'A' && c <= 'F') {
			digit = (unsigned int)(c - 'A') + 10U;
		} else {
			break;
		}

		value = (value << 4) | digit;
		len++;
	}

	if (len == 0) {
		return -EINVAL;
	}

	*out = (uint16_t)value;

	return len;
}

int api_parse_ipv6(const char *text, uint8_t out[16])
{
	if (text == NULL || out == NULL) {
		return -EINVAL;
	}

	uint8_t head[16] = {0};
	uint8_t tail[16] = {0};
	size_t head_len = 0;
	size_t tail_len = 0;
	bool seen_compression = false;
	uint8_t *part = head;
	size_t *part_len = &head_len;
	size_t pos = 0;

	/* A leading "::" is the only way an address may start with a colon. */
	if (text[0] == ':') {
		if (text[1] != ':') {
			return -EINVAL;
		}
		seen_compression = true;
		part = tail;
		part_len = &tail_len;
		pos = 2;
		/* "::" alone is the unspecified address. */
		if (text[pos] == '\0') {
			memset(out, 0, 16);
			return 0;
		}
	}

	while (text[pos] != '\0') {
		/*
		 * An embedded IPv4 tail, as in ::ffff:192.0.2.1. Recognised by
		 * scanning ahead for a dot before the next colon, since the
		 * group itself is ambiguous until then.
		 */
		bool is_ipv4 = false;

		for (size_t i = pos; text[i] != '\0' && text[i] != ':'; i++) {
			if (text[i] == '.') {
				is_ipv4 = true;
				break;
			}
		}

		if (is_ipv4) {
			uint8_t v4[4];

			if (*part_len + 4U > 16U || api_parse_ipv4(&text[pos], v4) != 0) {
				return -EINVAL;
			}
			memcpy(&part[*part_len], v4, 4);
			*part_len += 4U;
			pos += strlen(&text[pos]);
			break;
		}

		uint16_t group;
		int n = parse_hex_group(&text[pos], &group);

		if (n < 0 || *part_len + 2U > 16U) {
			return -EINVAL;
		}
		part[*part_len] = (uint8_t)(group >> 8);
		part[*part_len + 1U] = (uint8_t)(group & 0xFFU);
		*part_len += 2U;
		pos += (size_t)n;

		if (text[pos] == '\0') {
			break;
		}
		if (text[pos] != ':') {
			/* A zone id or any other suffix. Rejected, not trimmed. */
			return -EINVAL;
		}
		pos++;

		if (text[pos] == ':') {
			if (seen_compression) {
				return -EINVAL; /* Only one "::" is meaningful. */
			}
			seen_compression = true;
			part = tail;
			part_len = &tail_len;
			pos++;
			if (text[pos] == '\0') {
				break; /* A trailing "::". */
			}
		} else if (text[pos] == '\0') {
			return -EINVAL; /* A single trailing colon. */
		}
	}

	if (seen_compression) {
		/* The compression must stand for at least one zero group. */
		if (head_len + tail_len >= 16U) {
			return -EINVAL;
		}
	} else if (head_len != 16U) {
		return -EINVAL;
	}

	memset(out, 0, 16);
	memcpy(out, head, head_len);
	memcpy(&out[16U - tail_len], tail, tail_len);

	return 0;
}

bool api_ipv4_prefix_is_valid(uint8_t prefix_length)
{
	/*
	 * 1-30 per the IPv4Config schema. /31 and /32 are excluded by the
	 * contract: neither leaves room for a host and a gateway on a LAN this
	 * device can be managed from.
	 */
	return prefix_length >= 1U && prefix_length <= 30U;
}

static uint32_t to_u32(const uint8_t addr[4])
{
	return ((uint32_t)addr[0] << 24) | ((uint32_t)addr[1] << 16) | ((uint32_t)addr[2] << 8) |
	       (uint32_t)addr[3];
}

static uint32_t prefix_mask(uint8_t prefix_length)
{
	/* Shifting a 32-bit value by 32 is undefined, so /0 is handled apart. */
	return (prefix_length == 0U) ? 0U : (0xFFFFFFFFu << (32U - prefix_length));
}

bool api_ipv4_same_subnet(const uint8_t a[4], const uint8_t b[4], uint8_t prefix_length)
{
	if (a == NULL || b == NULL || prefix_length > 32U) {
		return false;
	}

	uint32_t mask = prefix_mask(prefix_length);

	return (to_u32(a) & mask) == (to_u32(b) & mask);
}

bool api_ipv4_is_usable_host(const uint8_t addr[4], uint8_t prefix_length)
{
	if (addr == NULL || !api_ipv4_prefix_is_valid(prefix_length)) {
		return false;
	}

	uint32_t value = to_u32(addr);
	uint32_t mask = prefix_mask(prefix_length);

	/* The subnet's own address and its broadcast belong to no host. */
	if ((value & ~mask) == 0U || (value & ~mask) == (~mask & 0xFFFFFFFFu)) {
		return false;
	}

	uint8_t first = addr[0];

	/*
	 * Ranges that are never a LAN host address. Each of these is accepted
	 * by the kernel and then simply does not work, which is a far worse
	 * failure than being told at configuration time.
	 */
	if (first == 0U) {
		return false; /* 0.0.0.0/8, "this network". */
	}
	if (first == 127U) {
		return false; /* Loopback. */
	}
	if (first >= 224U) {
		return false; /* Multicast and the reserved class E, 240/4. */
	}
	if (addr[0] == 169U && addr[1] == 254U) {
		return false; /* Link-local, which DHCP failure assigns by itself. */
	}

	return true;
}
