/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter-service's platform on the Matter SDK.
 *
 * Everything but schedule() runs on the Matter thread, inside work that
 * schedule() queued or inside a callback of the stack, so the stack lock is
 * already held and none is taken here.
 *
 * Decisions that are easy to undo by accident:
 *
 * - The window is announced over DNS-SD only. This board has no BLE, and the
 *   plan's first version commissions on the network the web interface set up.
 *
 * - Codes are generated from one read of the payload contents. The helpers
 *   that take rendezvous flags read the commissionable data again for each
 *   code, and each read goes through the settings file backend: on the board
 *   the debug shell's two such calls took seconds on the Matter thread.
 *
 * - A fabric's id is 16 hex digits of the SHA-256 of its root public key, a
 *   colon, and its fabric id. The root key is what identifies the trust
 *   anchor; the fabric id is chosen by a controller and the index is reused.
 *
 * - The window is re-read from a work item of its own, not inside the SDK's
 *   callback. When commissioning completes, CommissioningWindowManager::Cleanup()
 *   calls StopAdvertisement(), which tells the AppDelegate the window closed,
 *   and only then ResetState() marks it closed. Read inside the callback, the
 *   window was still open, and the web page showed it open with minutes left
 *   after the controller had finished (measured on the board in P3).
 *
 * - Calls into the SDK that the Matter thread may spend seconds in are timed
 *   and logged when slow: on this board they read the settings file.
 */

#include "matter_service_chip.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

#include <matter_service/matter_service.h>

LOG_MODULE_REGISTER(matter_service_chip, LOG_LEVEL_INF);

using namespace chip;
using chip::app::Clusters::AdministratorCommissioning::CommissioningWindowStatusEnum;

/* A fabric the SDK holds must never be left out of the list. */
BUILD_ASSERT(CHIP_CONFIG_MAX_FABRICS <= CONFIG_MATTER_SERVICE_MAX_FABRICS,
	     "CONFIG_MATTER_SERVICE_MAX_FABRICS is smaller than the SDK's fabric table");

namespace {

/* -- schedule: ScheduleWork carries one integer, the service needs two ----- */

struct Work
{
    void (*fn)(void * arg);
    void * arg;
    bool used;
};

/* The service's request slots, the start report, and window re-reads. */
Work sWork[CONFIG_MATTER_SERVICE_REQUEST_SLOTS + 3];
struct k_spinlock sWorkLock;

void RunWork(intptr_t index)
{
    k_spinlock_key_t key = k_spin_lock(&sWorkLock);
    Work work            = sWork[index];
    sWork[index].used    = false;
    k_spin_unlock(&sWorkLock, key);

    work.fn(work.arg);
}

int Schedule(void (*fn)(void * arg), void * arg)
{
    size_t index = ARRAY_SIZE(sWork);

    k_spinlock_key_t key = k_spin_lock(&sWorkLock);
    for (size_t i = 0; i < ARRAY_SIZE(sWork); i++)
    {
        if (!sWork[i].used)
        {
            sWork[i] = { fn, arg, true };
            index    = i;
            break;
        }
    }
    k_spin_unlock(&sWorkLock, key);

    if (index == ARRAY_SIZE(sWork))
    {
        return -ENOMEM;
    }

    CHIP_ERROR err = DeviceLayer::PlatformMgr().ScheduleWork(RunWork, static_cast<intptr_t>(index));
    if (err != CHIP_NO_ERROR)
    {
        key                = k_spin_lock(&sWorkLock);
        sWork[index].used = false;
        k_spin_unlock(&sWorkLock, key);
        return -EIO;
    }
    return 0;
}

int64_t NowMs()
{
    return k_uptime_get();
}

/* Log an SDK call that held the Matter thread for long. */
class SlowCallLog
{
public:
    explicit SlowCallLog(const char * what) : mWhat(what), mStart(k_uptime_get()) {}
    ~SlowCallLog()
    {
        int64_t ms = k_uptime_get() - mStart;
        if (ms >= 500)
        {
            LOG_WRN("%s took %lld ms on the Matter thread", mWhat, static_cast<long long>(ms));
        }
    }

private:
    const char * mWhat;
    int64_t mStart;
};

/* -- the window ------------------------------------------------------------- */

CommissioningWindowManager & Window()
{
    return Server::GetInstance().GetCommissioningWindowManager();
}

int OpenBasicWindow(uint32_t timeout_seconds)
{
    SlowCallLog slow("opening the commissioning window");
    CHIP_ERROR err = Window().OpenBasicCommissioningWindow(System::Clock::Seconds32(timeout_seconds),
                                                           CommissioningWindowAdvertisement::kDnssdOnly);
    if (err == CHIP_NO_ERROR)
    {
        return 0;
    }
    LOG_WRN("commissioning window not opened: %" CHIP_ERROR_FORMAT, err.Format());
    if (err == CHIP_ERROR_INCORRECT_STATE)
    {
        return -EBUSY;
    }
    if (err == CHIP_ERROR_INVALID_ARGUMENT)
    {
        return -EINVAL;
    }
    return -EIO;
}

void CloseWindow()
{
    Window().CloseCommissioningWindow();
}

void ReadWindow(struct matter_window_reading * out)
{
    CommissioningWindowManager & window = Window();

    *out                      = {};
    out->open                 = window.IsCommissioningWindowOpen();
    out->opened_by_controller = out->open && !window.GetOpenerVendorId().IsNull();
    switch (window.CommissioningWindowStatusForCluster())
    {
    case CommissioningWindowStatusEnum::kEnhancedWindowOpen:
        out->controller_mode = MATTER_WINDOW_MODE_ENHANCED;
        break;
    case CommissioningWindowStatusEnum::kBasicWindowOpen:
        out->controller_mode = MATTER_WINDOW_MODE_BASIC;
        break;
    default:
        out->controller_mode = MATTER_WINDOW_MODE_NONE;
        break;
    }
}

int ReadCodes(struct matter_codes * out)
{
    SlowCallLog slow("generating the onboarding codes");
    PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload, RendezvousInformationFlags(RendezvousInformationFlag::kOnNetwork));
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("onboarding payload unavailable: %" CHIP_ERROR_FORMAT, err.Format());
        return -EIO;
    }

    /* One byte short of each buffer, so the result stays a C string. */
    MutableCharSpan qr(out->qr_payload, sizeof(out->qr_payload) - 1);
    err = GetQRCode(qr, payload);
    if (err == CHIP_NO_ERROR)
    {
        out->qr_payload[qr.size()] = '\0';
        MutableCharSpan manual(out->manual_pairing_code, sizeof(out->manual_pairing_code) - 1);
        err = GetManualPairingCode(manual, payload);
        if (err == CHIP_NO_ERROR)
        {
            out->manual_pairing_code[manual.size()] = '\0';
        }
    }
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("onboarding codes not generated: %" CHIP_ERROR_FORMAT, err.Format());
        return -EIO;
    }

    snprintf(out->setup_passcode, sizeof(out->setup_passcode), "%08" PRIu32, payload.setUpPINCode);
    return 0;
}

/* -- the fabric table --------------------------------------------------------- */

void WriteId(const FabricTable & table, const FabricInfo & info, char * out, size_t size)
{
    Crypto::P256PublicKey root;
    uint8_t digest[Crypto::kSHA256_Hash_Length] = {};
    char root_hex[17]                            = "0000000000000000";

    if (table.FetchRootPubkey(info.GetFabricIndex(), root) == CHIP_NO_ERROR &&
        Crypto::Hash_SHA256(root.ConstBytes(), root.Length(), digest) == CHIP_NO_ERROR)
    {
        for (size_t i = 0; i < 8; i++)
        {
            snprintf(&root_hex[2 * i], 3, "%02X", digest[i]);
        }
    }
    else
    {
        LOG_WRN("fabric %u: root key unavailable", info.GetFabricIndex());
    }
    snprintf(out, size, "%s:%016" PRIX64, root_hex, info.GetFabricId());
}

size_t ReadFabrics(struct matter_fabric * out, size_t max)
{
    SlowCallLog slow("reading the fabric table");
    const FabricTable & table = Server::GetInstance().GetFabricTable();
    size_t n                  = 0;

    for (const FabricInfo & info : table)
    {
        if (n == max)
        {
            break;
        }
        struct matter_fabric & fabric = out[n++];
        CharSpan label                = info.GetFabricLabel();
        size_t label_len              = MIN(label.size(), sizeof(fabric.label) - 1);

        fabric              = {};
        fabric.fabric_index = info.GetFabricIndex();
        fabric.fabric_id    = info.GetFabricId();
        fabric.node_id      = info.GetNodeId();
        fabric.vendor_id    = static_cast<uint16_t>(info.GetVendorId());
        memcpy(fabric.label, label.data(), label_len);
        WriteId(table, info, fabric.id, sizeof(fabric.id));
    }
    return n;
}

uint32_t MinWindowSeconds()
{
    return Window().MinCommissioningTimeout().count();
}

uint32_t MaxWindowSeconds()
{
    return Window().MaxCommissioningTimeout().count();
}

/* -- what the stack reports ----------------------------------------------------- */

void RefreshWindow(void *)
{
    matter_service_report_window_changed();
}

/* After the SDK has finished handling what it is telling us about. */
void RefreshWindowLater()
{
    if (Schedule(RefreshWindow, nullptr) != 0)
    {
        LOG_WRN("window re-read not queued; reading it now");
        matter_service_report_window_changed();
    }
}

class ServiceAppDelegate : public ::AppDelegate
{
    void OnCommissioningWindowOpened() override { RefreshWindowLater(); }
    void OnCommissioningWindowClosed() override { RefreshWindowLater(); }
};

class ServiceFabricDelegate : public FabricTable::Delegate
{
    void OnFabricRemoved(const FabricTable &, FabricIndex) override { matter_service_report_fabrics_changed(); }
    void OnFabricCommitted(const FabricTable &, FabricIndex) override { matter_service_report_fabrics_changed(); }
    void OnFabricUpdated(const FabricTable &, FabricIndex) override { matter_service_report_fabrics_changed(); }
};

ServiceAppDelegate sAppDelegate;
ServiceFabricDelegate sFabricDelegate;

const struct matter_service_platform sPlatform = {
    .schedule           = Schedule,
    .now_ms             = NowMs,
    .open_basic_window  = OpenBasicWindow,
    .close_window       = CloseWindow,
    .read_window        = ReadWindow,
    .read_codes         = ReadCodes,
    .read_fabrics       = ReadFabrics,
    .min_window_seconds = MinWindowSeconds,
    .max_window_seconds = MaxWindowSeconds,
};

void ReportReady(void *)
{
    matter_service_report_started(0);
}

} // namespace

void matter_service_chip_init(void)
{
    int rc = matter_service_init(&sPlatform);
    if (rc != 0)
    {
        LOG_ERR("matter-service init failed: %d", rc);
    }
}

::AppDelegate * matter_service_chip_app_delegate()
{
    return &sAppDelegate;
}

void matter_service_chip_server_initialized()
{
    CHIP_ERROR err = Server::GetInstance().GetFabricTable().AddFabricDelegate(&sFabricDelegate);
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("fabric table changes will not be followed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void matter_service_chip_report_started(ChipError err)
{
    if (err != CHIP_NO_ERROR)
    {
        matter_service_report_started(static_cast<int32_t>(err.AsInteger()));
        return;
    }
    if (Schedule(ReportReady, nullptr) != 0)
    {
        LOG_ERR("the Matter thread did not take the start report");
        matter_service_report_started(-EIO);
    }
}
