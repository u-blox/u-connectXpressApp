/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <platform/ucxclient/DeviceInstanceInfoProviderImpl.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.ipp>

namespace chip {
namespace DeviceLayer {

// Explicit template instantiation to force compilation of all virtual methods
template class Internal::GenericDeviceInstanceInfoProvider<Internal::UcxclientConfig>;

} // namespace DeviceLayer
} // namespace chip
