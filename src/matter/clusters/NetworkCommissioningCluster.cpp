//
// Created by Kirill Shypachov on 19.04.2026.
//
#include "NetworkCommissioningCluster.h"
#include <app/clusters/network-commissioning/CodegenInstance.h>
#include <platform/NetworkCommissioning.h>

using namespace chip;
using namespace chip::DeviceLayer::NetworkCommissioning;

class EthernetNetworkIterator final : public NetworkIterator
{
public:
    size_t Count() override { return 1; }

    bool Next(Network & item) override
    {
        if (mEmitted) {
            return false;
        }

static constexpr uint8_t kNetworkId[] = { 'e', 't', 'h', '0' };

memcpy(item.networkID, kNetworkId, sizeof(kNetworkId));
item.networkIDLen = sizeof(kNetworkId);
item.connected = true;

mEmitted = true;
return true;
    }

void Release() override
{
    chip::Platform::Delete(this);
}

private:
bool mEmitted = false;
};

class CedarEthernetDriver final : public EthernetDriver
{
public:
    uint8_t GetMaxNetworks() override
    {
        return 1;
    }

    NetworkIterator * GetNetworks() override
    {
        return chip::Platform::New<EthernetNetworkIterator>();
    }

bool GetEnabled() override
{
    return true;
}
};

static CedarEthernetDriver sEthernetDriver;

static chip::app::Clusters::NetworkCommissioning::Instance sNetworkCommissioningInstance(
    0, // endpoint 0
    &sEthernetDriver
);

CHIP_ERROR matter_network_commissioning_init()
{
    return sNetworkCommissioningInstance.Init();
}