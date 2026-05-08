//
// Created by Kirill Shypachov on 19.04.2026.
//

#include "IdentifyCluster.h"

#include <app/DefaultTimerDelegate.h>
#include <app/clusters/identify-server/IdentifyCluster.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <lib/support/logging/CHIPLogging.h>

namespace {

using chip::EndpointId;
using chip::app::Clusters::IdentifyCluster;
using chip::app::Clusters::IdentifyDelegate;

class CedarIdentifyDelegate final : public IdentifyDelegate
{
public:
    void OnIdentifyStart(IdentifyCluster & cluster) override
    {
        ChipLogProgress(Zcl, "Identify start endpoint=%u", static_cast<unsigned>(cluster.GetPaths()[0].mEndpointId));
    }

    void OnIdentifyStop(IdentifyCluster & cluster) override
    {
        ChipLogProgress(Zcl, "Identify stop endpoint=%u", static_cast<unsigned>(cluster.GetPaths()[0].mEndpointId));
    }

    void OnTriggerEffect(IdentifyCluster & cluster) override
    {
        ChipLogProgress(Zcl, "Identify trigger effect endpoint=%u effect=%u variant=%u",
                        static_cast<unsigned>(cluster.GetPaths()[0].mEndpointId),
                        static_cast<unsigned>(cluster.GetEffectIdentifier()),
                        static_cast<unsigned>(cluster.GetEffectVariant()));
    }

    bool IsTriggerEffectEnabled() const override { return true; }
};

chip::app::DefaultTimerDelegate sTimerDelegate;
CedarIdentifyDelegate sIdentifyDelegate;

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint0(
    IdentifyCluster::Config(EndpointId{ 0 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint1(
    IdentifyCluster::Config(EndpointId{ 1 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint2(
    IdentifyCluster::Config(EndpointId{ 2 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint3(
    IdentifyCluster::Config(EndpointId{ 3 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint4(
    IdentifyCluster::Config(EndpointId{ 4 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint5(
        IdentifyCluster::Config(EndpointId{ 5 }, sTimerDelegate)
            .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
            .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint6(
    IdentifyCluster::Config(EndpointId{ 6 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

chip::app::RegisteredServerCluster<IdentifyCluster> sIdentifyClusterEndpoint7(
    IdentifyCluster::Config(EndpointId{ 7 }, sTimerDelegate)
        .WithIdentifyType(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator)
        .WithDelegate(&sIdentifyDelegate));

CHIP_ERROR register_identify_cluster(chip::app::RegisteredServerCluster<IdentifyCluster> & cluster, EndpointId endpointId)
{
    auto & registry = chip::app::CodegenDataModelProvider::Instance().Registry();

    if (registry.Get({ endpointId, chip::app::Clusters::Identify::Id }) != nullptr)
    {
        return CHIP_NO_ERROR;
    }

    return registry.Register(cluster.Registration());
}

} // namespace

CHIP_ERROR matter_identify_cluster_init()
{
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint0, EndpointId{ 0 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint1, EndpointId{ 1 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint2, EndpointId{ 2 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint3, EndpointId{ 3 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint4, EndpointId{ 4 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint5, EndpointId{ 5 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint6, EndpointId{ 6 }));
    ReturnErrorOnFailure(register_identify_cluster(sIdentifyClusterEndpoint7, EndpointId{ 7 }));

    return CHIP_NO_ERROR;
}
