/*
 *    Copyright (c) 2023 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

// u-connect-matter patch: key-logging for Wireshark decryption.
// u-connectXpressApp: only in a plug-in built for it ("ucxapp --matter --keylog --build" defines UCX_KEYLOG):
// the plug-in everybody else builds and ships has no code that writes a key to a file. In that build it is
// enabled at RUNTIME only when the environment variable UCX_KEYLOG_FILE is set
// to a writable path (e.g. run/matter-keys.log). Writes derived I2R/R2I AES-CCM
// session keys plus the HKDF salt/info so tools can decrypt captured traffic.
//
// SECURITY: leaks full session secrets. Opt-in only via env var; never log
// production keys unattended.
//
// File format (u-connect-matter keylog v1, one line per DeriveSessionKeys call):
//   <epoch_ms_utc> I2R=<32hex> R2I=<32hex> AC=<64hex> SALT=<hex> INFO=<hex>

#include <crypto/RawKeySessionKeystore.h>

#include <lib/support/BufferReader.h>

#include <cstdint>

// Only on host builds — no file I/O or getenv on embedded RTOS targets.
#if (defined(_WIN32) || defined(__linux__)) && defined(UCX_KEYLOG) && UCX_KEYLOG
#define UCX_KEYLOG_SUPPORTED 1
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#else
#define UCX_KEYLOG_SUPPORTED 0
#endif

namespace chip {
namespace Crypto {

// The underlying representation of the HKDF key handle
struct RawHkdfKeyHandle
{
    ByteSpan Span() const { return ByteSpan(data, size); }

    // Cap the data size so that the entire structure fits in the opaque context of the HKDF key handle.
    static constexpr size_t kMaxDataSize = std::min<size_t>(CHIP_CONFIG_HKDF_KEY_HANDLE_CONTEXT_SIZE - sizeof(uint8_t), UINT8_MAX);

    uint8_t data[kMaxDataSize];
    uint8_t size;
};

#if UCX_KEYLOG_SUPPORTED
namespace {

void HexAppend(char * buf, size_t & pos, size_t cap, const uint8_t * data, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len && pos + 2 < cap; ++i)
    {
        buf[pos++] = kHex[(data[i] >> 4) & 0xF];
        buf[pos++] = kHex[data[i] & 0xF];
    }
}

void UcxKeyLogDump(const uint8_t * i2r, const uint8_t * r2i, const uint8_t * ac, size_t acLen,
                   const uint8_t * salt, size_t saltLen, const uint8_t * info, size_t infoLen)
{
    const char * path = std::getenv("UCX_KEYLOG_FILE");
    if (path == nullptr || path[0] == '\0')
    {
        return;
    }

    // Serialize writes across sessions in case multiple threads derive keys.
    static std::mutex sMutex;
    std::lock_guard<std::mutex> lock(sMutex);

    FILE * fp = std::fopen(path, "ab");
    if (fp == nullptr)
    {
        return;
    }

    // Build one line in a stack buffer so we can do a single fwrite.
    char line[2048];
    size_t pos = 0;

    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    int n = std::snprintf(line + pos, sizeof(line) - pos, "%lld I2R=", ms);
    if (n > 0) pos += static_cast<size_t>(n);

    HexAppend(line, pos, sizeof(line), i2r, 16);
    n = std::snprintf(line + pos, sizeof(line) - pos, " R2I=");
    if (n > 0) pos += static_cast<size_t>(n);
    HexAppend(line, pos, sizeof(line), r2i, 16);

    n = std::snprintf(line + pos, sizeof(line) - pos, " AC=");
    if (n > 0) pos += static_cast<size_t>(n);
    HexAppend(line, pos, sizeof(line), ac, acLen);

    n = std::snprintf(line + pos, sizeof(line) - pos, " SALT=");
    if (n > 0) pos += static_cast<size_t>(n);
    HexAppend(line, pos, sizeof(line), salt, saltLen);

    n = std::snprintf(line + pos, sizeof(line) - pos, " INFO=");
    if (n > 0) pos += static_cast<size_t>(n);
    HexAppend(line, pos, sizeof(line), info, infoLen);

    if (pos + 1 < sizeof(line))
    {
        line[pos++] = '\n';
    }

    (void) std::fwrite(line, 1, pos, fp);
    std::fflush(fp);
    std::fclose(fp);
}

} // namespace
#endif // UCX_KEYLOG_SUPPORTED

CHIP_ERROR RawKeySessionKeystore::CreateKey(const Symmetric128BitsKeyByteArray & keyMaterial, Aes128KeyHandle & key)
{
    memcpy(key.AsMutable<Symmetric128BitsKeyByteArray>(), keyMaterial, sizeof(Symmetric128BitsKeyByteArray));
    return CHIP_NO_ERROR;
}

CHIP_ERROR RawKeySessionKeystore::CreateKey(const Symmetric128BitsKeyByteArray & keyMaterial, Hmac128KeyHandle & key)
{
    memcpy(key.AsMutable<Symmetric128BitsKeyByteArray>(), keyMaterial, sizeof(Symmetric128BitsKeyByteArray));
    return CHIP_NO_ERROR;
}

CHIP_ERROR RawKeySessionKeystore::CreateKey(const ByteSpan & keyMaterial, HkdfKeyHandle & key)
{
    RawHkdfKeyHandle & rawKey = key.AsMutable<RawHkdfKeyHandle>();

    VerifyOrReturnError(keyMaterial.size() <= sizeof(rawKey.data), CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(rawKey.data, keyMaterial.data(), keyMaterial.size());
    rawKey.size = static_cast<uint8_t>(keyMaterial.size());

    return CHIP_NO_ERROR;
}

CHIP_ERROR RawKeySessionKeystore::DeriveKey(const P256ECDHDerivedSecret & secret, const ByteSpan & salt, const ByteSpan & info,
                                            Aes128KeyHandle & key)
{
    HKDF_sha hkdf;

    return hkdf.HKDF_SHA256(secret.ConstBytes(), secret.Length(), salt.data(), salt.size(), info.data(), info.size(),
                            key.AsMutable<Symmetric128BitsKeyByteArray>(), sizeof(Symmetric128BitsKeyByteArray));
}

CHIP_ERROR RawKeySessionKeystore::DeriveSessionKeys(const ByteSpan & secret, const ByteSpan & salt, const ByteSpan & info,
                                                    Aes128KeyHandle & i2rKey, Aes128KeyHandle & r2iKey,
                                                    AttestationChallenge & attestationChallenge)
{
    HKDF_sha hkdf;
    SensitiveDataFixedBuffer<2 * sizeof(Symmetric128BitsKeyByteArray) + AttestationChallenge::Capacity()> keyMaterial;

    ReturnErrorOnFailure(hkdf.HKDF_SHA256(secret.data(), secret.size(), salt.data(), salt.size(), info.data(), info.size(),
                                          keyMaterial.Bytes(), keyMaterial.Capacity()));

    Encoding::LittleEndian::Reader reader(keyMaterial.Bytes(), keyMaterial.Capacity());

    CHIP_ERROR err =
        reader.ReadBytes(i2rKey.AsMutable<Symmetric128BitsKeyByteArray>(), sizeof(Symmetric128BitsKeyByteArray))
            .ReadBytes(r2iKey.AsMutable<Symmetric128BitsKeyByteArray>(), sizeof(Symmetric128BitsKeyByteArray))
            .ReadBytes(attestationChallenge.Bytes(), AttestationChallenge::Capacity())
            .StatusCode();

#if UCX_KEYLOG_SUPPORTED
    if (err == CHIP_NO_ERROR)
    {
        UcxKeyLogDump(i2rKey.As<Symmetric128BitsKeyByteArray>(),
                      r2iKey.As<Symmetric128BitsKeyByteArray>(),
                      attestationChallenge.Bytes(), AttestationChallenge::Capacity(),
                      salt.data(), salt.size(),
                      info.data(), info.size());
    }
#endif

    return err;
}

CHIP_ERROR RawKeySessionKeystore::DeriveSessionKeys(const HkdfKeyHandle & hkdfKey, const ByteSpan & salt, const ByteSpan & info,
                                                    Aes128KeyHandle & i2rKey, Aes128KeyHandle & r2iKey,
                                                    AttestationChallenge & attestationChallenge)
{
    return DeriveSessionKeys(hkdfKey.As<RawHkdfKeyHandle>().Span(), salt, info, i2rKey, r2iKey, attestationChallenge);
}

void RawKeySessionKeystore::DestroyKey(Symmetric128BitsKeyHandle & key)
{
    ClearSecretData(key.AsMutable<Symmetric128BitsKeyByteArray>());
}

void RawKeySessionKeystore::DestroyKey(HkdfKeyHandle & key)
{
    RawHkdfKeyHandle & rawKey = key.AsMutable<RawHkdfKeyHandle>();

    ClearSecretData(rawKey.data);
    rawKey.size = 0;
}

} // namespace Crypto
} // namespace chip
