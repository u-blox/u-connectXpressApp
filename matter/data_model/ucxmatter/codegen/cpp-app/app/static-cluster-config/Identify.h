// DO NOT EDIT - Generated file
//
// Application configuration for Identify based on EMBER configuration
// from E:\u-blox\u-connectXpressApp\matter\data_model\ucxmatter.matter
#pragma once

#include <app/util/cluster-config.h>
#include <clusters/Identify/AttributeIds.h>
#include <clusters/Identify/CommandIds.h>
#include <clusters/Identify/Enums.h>

#include <array>

namespace chip {
namespace app {
namespace Clusters {
namespace Identify {
namespace StaticApplicationConfig {
namespace detail {
inline constexpr AttributeId kEndpoint0EnabledAttributes[] = {
    Attributes::ClusterRevision::Id,
    Attributes::FeatureMap::Id,
    Attributes::IdentifyTime::Id,
    Attributes::IdentifyType::Id,
};
inline constexpr AttributeId kEndpoint1EnabledAttributes[] = {
    Attributes::ClusterRevision::Id,
    Attributes::FeatureMap::Id,
    Attributes::IdentifyTime::Id,
    Attributes::IdentifyType::Id,
};

inline constexpr CommandId kEndpoint1EnabledCommands[] = {
    Commands::Identify::Id,
};

inline constexpr AttributeId kEndpoint3EnabledAttributes[] = {
    Attributes::ClusterRevision::Id,
    Attributes::FeatureMap::Id,
    Attributes::IdentifyTime::Id,
    Attributes::IdentifyType::Id,
};

inline constexpr CommandId kEndpoint3EnabledCommands[] = {
    Commands::Identify::Id,
};

} // namespace detail

using FeatureBitmapType =
    Clusters::StaticApplicationConfig::NoFeatureFlagsDefined;

inline constexpr std::array<
    Clusters::StaticApplicationConfig::ClusterConfiguration<FeatureBitmapType>,
    3>
    kFixedClusterConfig = {{
        {
            .endpointNumber = 0,
            .featureMap = BitFlags<FeatureBitmapType>{},
            .enabledAttributes =
                Span<const AttributeId>(detail::kEndpoint0EnabledAttributes),
            .enabledCommands = Span<const CommandId>(),
        },
        {
            .endpointNumber = 1,
            .featureMap = BitFlags<FeatureBitmapType>{},
            .enabledAttributes =
                Span<const AttributeId>(detail::kEndpoint1EnabledAttributes),
            .enabledCommands =
                Span<const CommandId>(detail::kEndpoint1EnabledCommands),
        },
        {
            .endpointNumber = 3,
            .featureMap = BitFlags<FeatureBitmapType>{},
            .enabledAttributes =
                Span<const AttributeId>(detail::kEndpoint3EnabledAttributes),
            .enabledCommands =
                Span<const CommandId>(detail::kEndpoint3EnabledCommands),
        },
    }};

// If a specific attribute is supported at all across all endpoint static
// instantiations
inline constexpr bool
IsAttributeEnabledOnSomeEndpoint(AttributeId attributeId) {
  switch (attributeId) {
  case Attributes::ClusterRevision::Id:
  case Attributes::FeatureMap::Id:
  case Attributes::IdentifyTime::Id:
  case Attributes::IdentifyType::Id:
    return true;
  default:
    return false;
  }
}

// If a specific command is supported at all across all endpoint static
// instantiations
inline constexpr bool IsCommandEnabledOnSomeEndpoint(CommandId commandId) {
  switch (commandId) {
  case Commands::Identify::Id:
    return true;
  default:
    return false;
  }
}

} // namespace StaticApplicationConfig
} // namespace Identify
} // namespace Clusters
} // namespace app
} // namespace chip
