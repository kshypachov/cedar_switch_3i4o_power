//
// Created by Kirill Shypachov on 11.04.2026.
//

#include "matter_init.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include "matter_event_loop.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <data-model-providers/codegen/Instance.h>
#include <app/server/DefaultAclStorage.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <zap-generated/CodeDrivenCallback.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <crypto/PSAOperationalKeystore.h>


#include <app/clusters/network-commissioning/CodegenInstance.h>
#include <platform/NetworkCommissioning.h>
#include "clusters/IdentifyCluster.h"
#include "clusters/NetworkCommissioningCluster.h"
#include "matter_service_chip.h"

#include <matter_service/matter_service.h>

LOG_MODULE_REGISTER(matter);

static struct k_work matter_start_work;
static atomic_t matter_start_work_submitted;

K_THREAD_STACK_DEFINE(matter_boot_thread_stack, 8192);
static struct k_thread matter_boot_thread_data;
#define MATTER_BOOT_THREAD_PRIORITY   14

using namespace chip;
using namespace chip::DeviceLayer;

static chip::CommonCaseDeviceServerInitParams serverInitParams;
//static chip::Crypto::PSAOperationalKeystore sPSAOperationalKeystore;


static bool initialized = false;
static bool started = false;

static void log_matter_onboarding_codes()
{
    char qrCodeBuffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
    chip::MutableCharSpan qrCode(qrCodeBuffer);

    CHIP_ERROR err = GetQRCode(
        qrCode,
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork)
    );

    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to generate Matter QR code: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    char manualCodeBuffer[chip::kManualSetupLongCodeCharLength + 1] = {};
    chip::MutableCharSpan manualCode(manualCodeBuffer);

    err = GetManualPairingCode(
        manualCode,
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork)
    );

    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to generate Matter manual pairing code: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    LOG_INF("Matter QR code: %s", qrCodeBuffer);
    LOG_INF("Matter manual pairing code: %s", manualCodeBuffer);
}

static CHIP_ERROR matter_init()
{
    CHIP_ERROR err;

    if (initialized) {
        LOG_INF("Matter already initialized");
        return CHIP_NO_ERROR;
    }

    LOG_INF("Init CHIP stack");

    err = chip::Platform::MemoryInit();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("Platform::MemoryInit() failed");
        return err;
    }


    // 1. Init platform
    err = PlatformMgr().InitChipStack();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Matter init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    chip::Credentials::SetDeviceAttestationCredentialsProvider(
    chip::Credentials::Examples::GetExampleDACProvider());

    err = serverInitParams.InitializeStaticResourcesBeforeServerInit();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to init static resources: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    serverInitParams.dataModelProvider = chip::app::CodegenDataModelProviderInstance(serverInitParams.persistentStorageDelegate);
    // matter-service follows the commissioning window through these callbacks.
    serverInitParams.appDelegate = matter_service_chip_app_delegate();

    err = matter_network_commissioning_init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("NetworkCommissioning init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    err = matter_identify_cluster_init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("IdentifyCluster init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    err = chip::Server::GetInstance().Init(serverInitParams);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Matter Server Init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    matter_service_chip_server_initialized();

    initialized = true;
    LOG_INF("Matter stack initialized");

    return CHIP_NO_ERROR;
}

static CHIP_ERROR matter_start()
{
    CHIP_ERROR err;

    if (started) {
        LOG_INF("Matter already started");
        return CHIP_NO_ERROR;
    }

    err = PlatformMgr().StartEventLoopTask();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to start Matter loop: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    started = true;
    LOG_INF("Matter event loop started");

    // err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
    // if (err != CHIP_NO_ERROR) {
    //     LOG_ERR("OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
    //     return;
    // }
    //
    // chip::CommissioningWindowManager & cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
    //
    // if (cwm.IsCommissioningWindowOpen()) {
    //     LOG_INF("Commissioning window is open");
    //     log_matter_onboarding_codes();
    // } else {
    //     LOG_INF("Commissioning window is closed");
    // }

    return CHIP_NO_ERROR;
}

static void matter_start_work_handler(void * p1, void * p2, void * p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    matter_service_report_starting();

    CHIP_ERROR err = matter_init();
    if (err == CHIP_NO_ERROR) {
        err = matter_start();
    }
    matter_service_chip_report_started(err);

    start_io_to_matter_stream();

    k_thread_abort(k_current_get());
}

void start_matter(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(mgmt_event);
    ARG_UNUSED(iface);

    // A net_mgmt callback runs on the event thread every other subscriber
    // shares: it only starts the boot thread, and never waits (plan section 2;
    // it used to sleep 5 s here).
    LOG_INF("IPv6 address added, starting Matter");

    if (!atomic_cas(&matter_start_work_submitted, 0, 1)) {
        LOG_INF("Matter start work already submitted");
        return;
    }

    // Start Matter boot task

    k_tid_t tid = k_thread_create(
        &matter_boot_thread_data,
        matter_boot_thread_stack,
        K_THREAD_STACK_SIZEOF(matter_boot_thread_stack),
        matter_start_work_handler,
        NULL, NULL, NULL,
        MATTER_BOOT_THREAD_PRIORITY,
        0, K_NO_WAIT);

    k_thread_name_set(tid, "matter_boot_thread");

}
