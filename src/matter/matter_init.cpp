//
// Created by Kirill Shypachov on 11.04.2026.
//

#include "matter_init.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>

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

LOG_MODULE_REGISTER(matter);

static struct k_work matter_start_work;
static atomic_t matter_start_work_submitted;

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

void matter_init()
{
    CHIP_ERROR err;

    if (initialized) {
        LOG_INF("Matter already initialized");
        return;
    }

    LOG_INF("Init CHIP stack");

    err = chip::Platform::MemoryInit();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("Platform::MemoryInit() failed");
        return;
    }


    // 1. Init platform
    err = PlatformMgr().InitChipStack();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Matter init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    chip::Credentials::SetDeviceAttestationCredentialsProvider(
    chip::Credentials::Examples::GetExampleDACProvider());

    err = serverInitParams.InitializeStaticResourcesBeforeServerInit();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to init static resources: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    serverInitParams.dataModelProvider = chip::app::CodegenDataModelProviderInstance(serverInitParams.persistentStorageDelegate);

    err = matter_network_commissioning_init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("NetworkCommissioning init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    err = matter_identify_cluster_init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("IdentifyCluster init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    err = chip::Server::GetInstance().Init(serverInitParams);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Matter Server Init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    initialized = true;
    LOG_INF("Matter stack initialized");


}

void matter_start()
{
    CHIP_ERROR err;

    if (started) {
        LOG_INF("Matter already started");
        return;
    }

    err = PlatformMgr().StartEventLoopTask();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("Failed to start Matter loop: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    started = true;
    LOG_INF("Matter event loop started");

    err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    chip::CommissioningWindowManager & cwm = chip::Server::GetInstance().GetCommissioningWindowManager();

    if (cwm.IsCommissioningWindowOpen()) {
        LOG_INF("Commissioning window is open");
        log_matter_onboarding_codes();
    } else {
        LOG_INF("Commissioning window is closed");
    }
}

static void matter_start_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    matter_init();
    matter_start();
}

void init_ip_v6_address(struct net_mgmt_event_callback *cb,
                     uint64_t mgmt_event,
                     struct net_if *iface)
{
    LOG_INF("Default interface: %p", iface);

    if (!atomic_cas(&matter_start_work_submitted, 0, 1)) {
        LOG_INF("Matter start work already submitted");
        return;
    }

    k_work_init(&matter_start_work, matter_start_work_handler);
    int ret = k_work_submit(&matter_start_work);
    if (ret < 0) {
        atomic_clear(&matter_start_work_submitted);
        LOG_ERR("Failed to submit Matter start work: %d", ret);
    }
}
