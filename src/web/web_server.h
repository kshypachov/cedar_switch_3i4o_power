/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_WEB_SERVER_H_
#define APP_WEB_SERVER_H_

/**
 * @brief Start the web interface: authentication, API v1 and the browser
 *        application on port 80, IPv4 and IPv6.
 *
 * Call after setting_registry_init(): the administrator verifier is read from
 * settings-registry here.
 *
 * @return 0, or the negative errno of http_server_start().
 */
int app_web_init(void);

#endif /* APP_WEB_SERVER_H_ */
