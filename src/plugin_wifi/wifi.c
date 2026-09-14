//
// Created by Kirill Shypachov on 17.03.2026.
//
// P5: the C6's EN and BOOT lines belong to coprocessor-manager, which also owns
// the C6's UART and must see every reset (a new log generation, a reset marker,
// and no reset while a network apply or the flasher holds the chip). These
// debug entry points stay and go through it. The pulse sequence they used to
// drive by hand - EN low 100 ms; for download mode BOOT held low and released
// 200 ms after EN - is the one src/services/coprocessor now drives.
//

#include "wifi.h"
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <coprocessor_manager/coprocessor_manager.h>

LOG_MODULE_REGISTER(wifi, CONFIG_LOG_DEFAULT_LEVEL);

/* A normal reset of the C6: it starts its firmware. */
int wifi_reset_simple()
{
	int ret = coprocessor_manager_reset(false);

	if (ret != 0) {
		LOG_ERR("Error %d: the C6 reset was refused or failed", ret);
	}
	return ret;
}

/*
 * Reset the C6 with its BOOT strap held: it starts the ROM loader and waits for
 * a download ("waiting for download" on its UART). Nothing is written.
 */
void wifi_init() {
    LOG_INF("Resetting the C6 into its ROM loader...");
    int ret = coprocessor_manager_reset(true);

    if (ret != 0) {
        LOG_ERR("Error %d: the C6 reset was refused or failed", ret);
        return;
    }
    LOG_INF("C6 released with BOOT held: ROM download mode");
}
