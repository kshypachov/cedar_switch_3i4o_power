//
// Created by Kirill Shypachov on 19.04.2026.
//

#include "shell.h"

#include "crypto_bench.h"

#include <errno.h>
#include <stdlib.h>

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_SHELL)

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(matter_shell);

static void log_onboarding_codes()
{
    char qr_code_buffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
    chip::MutableCharSpan qr_code(qr_code_buffer);

    CHIP_ERROR err = GetQRCode(qr_code, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to generate Matter QR code: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    char manual_code_buffer[chip::kManualSetupLongCodeCharLength + 1] = {};
    chip::MutableCharSpan manual_code(manual_code_buffer);

    err = GetManualPairingCode(manual_code, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to generate Matter manual pairing code: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    LOG_INF("Matter QR code: %s", qr_code_buffer);
    LOG_INF("Matter manual pairing code: %s", manual_code_buffer);
}

static void reset_all_fabrics_work(intptr_t)
{
    auto & server = chip::Server::GetInstance();
    const uint8_t fabric_count = server.GetFabricTable().FabricCount();

    LOG_INF("Deleting all Matter fabrics, count=%u", fabric_count);

    if (server.GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
        server.GetCommissioningWindowManager().CloseCommissioningWindow();
    }

    server.GetFabricTable().DeleteAllFabrics();

    LOG_INF("Matter fabrics after delete: %u", server.GetFabricTable().FabricCount());
}

static void commissioning_window_status_work(intptr_t)
{
    auto & server = chip::Server::GetInstance();
    auto & cwm = server.GetCommissioningWindowManager();

    LOG_INF("Matter fabric count: %u", server.GetFabricTable().FabricCount());
    LOG_INF("Commissioning window is %s", cwm.IsCommissioningWindowOpen() ? "open" : "closed");
}

static void open_basic_commissioning_window_work(intptr_t)
{
    auto & server = chip::Server::GetInstance();
    auto & cwm = server.GetCommissioningWindowManager();

    if (cwm.IsCommissioningWindowOpen()) {
        LOG_INF("Commissioning window is already open");
        log_onboarding_codes();
        return;
    }

    CHIP_ERROR err = cwm.OpenBasicCommissioningWindow();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    LOG_INF("Commissioning window is open");
    log_onboarding_codes();
}

static int schedule_matter_work(const struct shell *sh, chip::DeviceLayer::AsyncWorkFunct work)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(work, 0);
    if (err != CHIP_NO_ERROR) {
        shell_error(sh, "Failed to schedule Matter work: %" CHIP_ERROR_FORMAT, err.Format());
        return -EIO;
    }

    return 0;
}

static int cmd_matter_fabric_count(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Matter fabric count: %u", chip::Server::GetInstance().GetFabricTable().FabricCount());
    return 0;
}

static int cmd_matter_fabric_reset(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(reset_all_fabrics_work, 0);
    if (err != CHIP_NO_ERROR) {
        shell_error(sh, "Failed to schedule fabric reset: %" CHIP_ERROR_FORMAT, err.Format());
        return -EIO;
    }

    shell_print(sh, "Matter fabric reset scheduled");
    return 0;
}

static int cmd_matter_commissioning_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = schedule_matter_work(sh, commissioning_window_status_work);
    if (ret == 0) {
        shell_print(sh, "Matter commissioning status scheduled");
    }

    return ret;
}

static int cmd_matter_commissioning_open(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = schedule_matter_work(sh, open_basic_commissioning_window_work);
    if (ret == 0) {
        shell_print(sh, "Matter commissioning window open scheduled");
    }

    return ret;
}

static int cmd_matter_crypto_bench(const struct shell *sh, size_t argc, char **argv)
{
    size_t iterations = 10;

    if (argc > 1) {
        char *end = nullptr;
        unsigned long value = strtoul(argv[1], &end, 10);
        if ((end == argv[1]) || (*end != '\0')) {
            shell_error(sh, "Invalid iteration count: %s", argv[1]);
            return -EINVAL;
        }

        iterations = static_cast<size_t>(value);
    }

    return matter_crypto_bench_run(sh, iterations);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    matter_fabric_cmds,
    SHELL_CMD(count, NULL, "Print Matter fabric count.", cmd_matter_fabric_count),
    SHELL_CMD(reset, NULL, "Delete all Matter fabrics.", cmd_matter_fabric_reset),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
    matter_commissioning_cmds,
    SHELL_CMD(status, NULL, "Print commissioning window status.", cmd_matter_commissioning_status),
    SHELL_CMD(open, NULL, "Open basic commissioning window.", cmd_matter_commissioning_open),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
    matter_cmds,
    SHELL_CMD(commissioning, &matter_commissioning_cmds, "Matter commissioning commands.", NULL),
    SHELL_CMD_ARG(crypto_bench, NULL, "Run PSA crypto benchmark: matter crypto_bench [iterations].",
                  cmd_matter_crypto_bench, 1, 1),
    SHELL_CMD(fabric, &matter_fabric_cmds, "Matter fabric commands.", NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(matter, &matter_cmds, "Matter debug commands.", NULL);

#endif
