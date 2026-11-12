// DO NOT EDIT - Generated file
//
// Application configuration for GroupKeyManagement based on EMBER configuration
// from E:\u-blox\u-connectXpressApp\matter\data_model\ucxmatter.matter
#pragma once

#include <app/util/cluster-config.h>
#include <clusters/GroupKeyManagement/AttributeIds.h>
#include <clusters/GroupKeyManagement/CommandIds.h>
#include <clusters/GroupKeyManagement/Enums.h>

#include <array>

namespace chip {
namespace app {
namespace Clusters {
namespace GroupKeyManagement {
namespace StaticApplicationConfig {
namespace detail {
inline constexpr AttributeId kEndpoint0EnabledAttributes[] = {
    Attributes::ClusterRevision::Id,       Attributes::FeatureMap::Id,
    Attributes::GroupKeyMap::Id,           Attributes::GroupTable::Id,
    Attributes::MaxGroupKeysPerFabric::Id, Attributes::MaxGroupsPerFabric::Id,
};
} // namespace detail

using FeatureBitmapType = Feature;

inline constexpr std::array<
    Clusters::StaticApplicationConfig::ClusterConfiguration<FeatureBitmapType>,
    1>
    kFixedClusterConfig = {{
        {
            .endpointNumber = 0,
            .featureMap = BitFlags<FeatureBitmapType>{},
            .enabledAttributes =
                Span<const AttributeId>(detail::kEndpoint0EnabledAttributes),
            .enabledCommands = Span<const CommandId>(),
        },
    }};

// If a specific attribute is supported at all across all endpoint static
// instantiations
inline constexpr bool
IsAttributeEnabledOnSomeEndpoint(AttributeId attributeId) {
  switch (attributeId) {
  case Attributes::ClusterRevision::Id:
  case Attributes::FeatureMap::Id:
  case Attributes::GroupKeyMap::Id:
  case Attributes::GroupTable::Id:
  case Attributes::MaxGroupKeysPerFabric::Id:
  case Attributes::MaxGroupsPerFabric::Id:
    return true;
  default:
    return false;
  }
}

// If a specific command is supported at all across all endpoint static
// instantiations
inline constexpr bool IsCommandEnabledOnSomeEndpoint(CommandId commandId) {
  switch (commandId) {
  default:
    return false;
  }
}

} // namespace StaticApplicationConfig
} // namespace GroupKeyManagement
} // namespace Clusters
} // namespace app
} // namespace chip
