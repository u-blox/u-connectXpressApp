/* What the SDK's data model code calls back into the application. The SDK ships each of these as a weak default
 * (gen/app/callback-stub.cpp, src/app/util), and the PE linker of MinGW does not take a weak definition from
 * another object file: without the definitions here the plug-in does not link on Windows. One cluster init for
 * each server cluster of data_model/ucxmatter.zap: a cluster added there needs its line here. */
#include <app-common/zap-generated/callback.h>
#include <app/util/generic-callbacks.h>

#include "node.h"

using namespace chip;
using chip::Protocols::InteractionModel::Status;

// The clusters start in the SDK's own code (Matter...ClusterInitCallback); nothing to add for an endpoint
void emberAfAccessControlClusterInitCallback(EndpointId) {}
void emberAfAdministratorCommissioningClusterInitCallback(EndpointId) {}
void emberAfBasicInformationClusterInitCallback(EndpointId) {}
void emberAfBooleanStateClusterInitCallback(EndpointId) {}
void emberAfDescriptorClusterInitCallback(EndpointId) {}
void emberAfGeneralCommissioningClusterInitCallback(EndpointId) {}
void emberAfGeneralDiagnosticsClusterInitCallback(EndpointId) {}
void emberAfGroupKeyManagementClusterInitCallback(EndpointId) {}
void emberAfGroupsClusterInitCallback(EndpointId) {}
void emberAfIdentifyClusterInitCallback(EndpointId) {}
void emberAfLevelControlClusterInitCallback(EndpointId) {}
void emberAfNetworkCommissioningClusterInitCallback(EndpointId) {}
void emberAfOnOffClusterInitCallback(EndpointId) {}
void emberAfOperationalCredentialsClusterInitCallback(EndpointId) {}
void emberAfTemperatureMeasurementClusterInitCallback(EndpointId) {}

// Every attribute of the data model may be read and written (the access control list decides who may)
bool emberAfAttributeReadAccessCallback(EndpointId, ClusterId, AttributeId)
{
    return true;
}

bool emberAfAttributeWriteAccessCallback(EndpointId, ClusterId, AttributeId)
{
    return true;
}

// No attribute is kept outside the SDK's own storage
Status emberAfExternalAttributeReadCallback(EndpointId, ClusterId, const EmberAfAttributeMetadata *, uint8_t *, uint16_t)
{
    return Status::Failure;
}

Status emberAfExternalAttributeWriteCallback(EndpointId, ClusterId, const EmberAfAttributeMetadata *, uint8_t *)
{
    return Status::Failure;
}

Status MatterPreAttributeChangeCallback(const app::ConcreteAttributePath &, uint8_t, uint16_t, uint8_t *)
{
    return Status::Success;
}

void MatterPostAttributeChangeCallback(const app::ConcreteAttributePath & path, uint8_t, uint16_t size, uint8_t * value)
{
    ucxm::NodeAttributeChanged(path.mEndpointId, path.mClusterId, path.mAttributeId, value, size);
}
