#include <zephyr/shell/shell.h>

#include "wifi.h"

static int cmd_wifi_ctrl_init(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	wifi_init();
	shell_print(sh, "wifi_init() executed");
	return 0;
}

static int cmd_wifi_ctrl_reset(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = wifi_reset_simple();
	if (ret != 0) {
		shell_error(sh, "wifi_reset_simple() failed: %d", ret);
		return ret;
	}

	shell_print(sh, "ESP32 reset pulse done");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wifi_ctrl_cmds,
	SHELL_CMD(init, NULL, "Run wifi_init()", cmd_wifi_ctrl_init),
	SHELL_CMD(reset, NULL, "Pulse ESP32 reset pin: low->delay->high.", cmd_wifi_ctrl_reset),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wifi_ctrl, &wifi_ctrl_cmds, "WiFi control commands.", NULL);
