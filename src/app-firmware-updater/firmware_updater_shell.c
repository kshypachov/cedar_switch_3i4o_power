#include <zephyr/shell/shell.h>

#include "firmware_updater.h"

static int cmd_fwupd_fetch_prjconf(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Downloading prj.conf from GitHub...");
	ret = firmware_updater_fetch_and_print_prjconf();
	if (ret != 0) {
		shell_error(sh, "Download failed: %d", ret);
		return ret;
	}

	shell_print(sh, "Download done.");
	return 0;
}

static int cmd_fwupd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = firmware_updater_confirm_image();
	if (ret != 0) {
		shell_error(sh, "Image confirm failed: %d", ret);
		return ret;
	}

	shell_print(sh, "Image confirmed.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fwupd_cmds,
	SHELL_CMD(fetch_prjconf, NULL, "Download and print remote prj.conf.", cmd_fwupd_fetch_prjconf),
	SHELL_CMD(confirm, NULL, "Confirm currently running image (MCUboot).", cmd_fwupd_confirm),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fwupd, &fwupd_cmds, "Firmware updater commands.", NULL);
