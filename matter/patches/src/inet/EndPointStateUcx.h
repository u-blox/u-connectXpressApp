/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 */

/**
 * @file
 *   Endpoint state for ucxclient (NORA-W36 sockets).
 *   Minimal state tracking since NORA-W36 manages actual sockets.
 */

#pragma once

namespace chip {
namespace Inet {

/**
 * State management for ucxclient endpoints.
 * Similar to EndPointStateSockets but for NORA-W36 sockets.
 */
class EndPointStateUcx
{
protected:
    EndPointStateUcx() = default;
    
    // NORA-W36 socket handle (-1 = invalid)
    int32_t mSocket = -1;
    
    bool IsSocketValid() const { return mSocket >= 0; }
};

} // namespace Inet
} // namespace chip
