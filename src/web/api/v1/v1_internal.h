/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Declarations shared by the v1 binding files and the route table.
 */

#ifndef WEB_API_V1_INTERNAL_H_
#define WEB_API_V1_INTERNAL_H_

#include <job_manager/job_manager.h>
#include <web_api/web_api.h>
#include <web_auth/web_auth.h>

#include "web_api_v1.h"

#define V1_NO_BODY   0U
#define V1_BODY(type) sizeof(type)

/* SetupRequest */
struct v1_setup_body {
	char password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* LoginRequest */
struct v1_login_body {
	char password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* PasswordRequest */
struct v1_password_body {
	char current_password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
	char new_password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* CommissioningRequest; mode is checked against its enum, so 16 bytes hold
 * every value a client might send before it is refused. */
struct v1_commissioning_body {
	char mode[16];
	int64_t timeout_seconds;
};

extern const struct web_json_object v1_setup_schema;
extern const struct web_json_object v1_login_schema;
extern const struct web_json_object v1_password_schema;
extern const struct web_json_object v1_commissioning_schema;

void v1_get_auth_state(struct web_api_call *call);
void v1_setup_admin(struct web_api_call *call);
void v1_login(struct web_api_call *call);
void v1_get_session(struct web_api_call *call);
void v1_logout(struct web_api_call *call);
void v1_change_password(struct web_api_call *call);
void v1_get_system_status(struct web_api_call *call);
void v1_get_capabilities(struct web_api_call *call);
void v1_get_job(struct web_api_call *call);
void v1_get_matter_status(struct web_api_call *call);
void v1_get_commissioning_window(struct web_api_call *call);
void v1_open_commissioning_window(struct web_api_call *call);
void v1_close_commissioning_window(struct web_api_call *call);
void v1_get_onboarding_codes(struct web_api_call *call);
void v1_list_fabrics(struct web_api_call *call);

/** The identity given to web_api_v1_init(). */
const struct web_api_v1_identity *v1_identity(void);

/** Start the worker that runs password changes (auth.c). */
int v1_auth_worker_start(void);

/** The resource a job of @p kind acts on, or NULL. */
const char *v1_job_resource_url(enum job_kind kind);

#endif /* WEB_API_V1_INTERNAL_H_ */
