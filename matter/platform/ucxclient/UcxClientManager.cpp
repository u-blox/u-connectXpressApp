#include "UcxClientManager.h"

#include "UcxStore.h"
#include "VerboseLogging.h"
#include "support/DeviceConfig.h"
#include "ucxm_host.h"

#include <inet/UDPEndPointImplUcx.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace ucxmatter {

DeviceConfig & GetDeviceConfig()
{
    static DeviceConfig sConfig;
    return sConfig;
}

// One blob: [key length][key][value length][value]..., lengths as 32 bits in host order (u-connectMatter's files)
bool StoreLoad(const char * name, BlobMap & map)
{
    const ucxapp_matter_host_t * host = gUcxmHost;
    int32_t length                    = host != nullptr ? host->storeLoad(name, nullptr, 0) : -1;

    map.clear();
    if (length <= 0)
    {
        return false;
    }
    std::vector<uint8_t> blob(static_cast<size_t>(length));
    if (host->storeLoad(name, blob.data(), blob.size()) != length)
    {
        return false;
    }
    size_t at = 0;
    while (at + sizeof(uint32_t) <= blob.size())
    {
        uint32_t keyLength;
        uint32_t valueLength;

        memcpy(&keyLength, &blob[at], sizeof(keyLength));
        at += sizeof(keyLength);
        if (keyLength == 0 || keyLength > 256 || blob.size() - at < keyLength + sizeof(valueLength))
        {
            break;
        }
        std::string key(reinterpret_cast<const char *>(&blob[at]), keyLength);
        at += keyLength;
        memcpy(&valueLength, &blob[at], sizeof(valueLength));
        at += sizeof(valueLength);
        if (blob.size() - at < valueLength)
        {
            break;
        }
        map[key].assign(blob.begin() + static_cast<std::ptrdiff_t>(at),
                        blob.begin() + static_cast<std::ptrdiff_t>(at + valueLength));
        at += valueLength;
    }
    return true;
}

bool StoreSave(const char * name, const BlobMap & map)
{
    const ucxapp_matter_host_t * host = gUcxmHost;
    std::vector<uint8_t> blob;

    for (const auto & entry : map)
    {
        const uint32_t keyLength   = static_cast<uint32_t>(entry.first.size());
        const uint32_t valueLength = static_cast<uint32_t>(entry.second.size());
        const uint8_t * lengths[]  = { reinterpret_cast<const uint8_t *>(&keyLength),
                                       reinterpret_cast<const uint8_t *>(&valueLength) };

        blob.insert(blob.end(), lengths[0], lengths[0] + sizeof(keyLength));
        blob.insert(blob.end(), entry.first.begin(), entry.first.end());
        blob.insert(blob.end(), lengths[1], lengths[1] + sizeof(valueLength));
        blob.insert(blob.end(), entry.second.begin(), entry.second.end());
    }
    return host != nullptr && host->storeSave(name, blob.data(), blob.size());
}

void StoreRemove(const char * name)
{
    if (gUcxmHost != nullptr)
    {
        (void) gUcxmHost->storeRemove(name);
    }
}

} // namespace ucxmatter

namespace chip {
namespace DeviceLayer {

std::atomic<bool> gVerboseLogging{ false };

namespace {

std::mutex sUrcMutex;
std::deque<std::string> sUrcQueue;
std::atomic<int32_t> sOwnedGatt[3]{ { -1 }, { -1 }, { -1 } };

// "+UEBTGCW:0,42,..." (write), "+UEBTGRR:0,42" (read), "+UEBTGIC:0,42" (indication confirmed): the second number
bool IsOwnedGattUrc(const char * line, size_t length)
{
    static const char * const kNames[] = { "+UEBTGCW:", "+UEBTGRR:", "+UEBTGIC:" };

    for (const char * name : kNames)
    {
        const size_t n = strlen(name);

        if (length > n && strncmp(line, name, n) == 0)
        {
            const char * comma = static_cast<const char *>(memchr(line + n, ',', length - n));
            const int32_t handle = comma != nullptr ? static_cast<int32_t>(strtol(comma + 1, nullptr, 10)) : -1;

            for (const auto & owned : sOwnedGatt)
            {
                if (handle >= 0 && owned.load() == handle)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

UcxClientManager & UcxClientManager::GetInstance()
{
    static UcxClientManager sInstance;
    return sInstance;
}

uCxHandle_t * UcxClientManager::GetUcxHandle()
{
    return ucxm_host_handle();
}

bool UcxClientManager::QueueUrc(const char * line, size_t length)
{
    {
        std::lock_guard<std::mutex> lock(sUrcMutex);

        MdnsNoteUrc(line, length);
        sUrcQueue.emplace_back(line, length);
    }
    System::UcxWake(); // the Matter thread takes it now, not at its next timer
    return IsOwnedGattUrc(line, length) || OwnsSocketUrc(line, length);
}

void UcxClientManager::ProcessUrcs()
{
    for (;;)
    {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(sUrcMutex);

            if (sUrcQueue.empty())
            {
                break;
            }
            line = std::move(sUrcQueue.front());
            sUrcQueue.pop_front();
        }
        // The callbacks send AT commands: never with the queue locked, the AT client's thread fills it
        ucxm_host_urc(&line[0], line.size());
    }
    // What the SDK asked to send while it handled the above
    Inet::UDPEndPointImplUcx::ProcessTxQueue();
}

void UcxClientManager::OwnGattHandles(int32_t c1, int32_t c2, int32_t c2Cccd)
{
    sOwnedGatt[0] = c1;
    sOwnedGatt[1] = c2;
    sOwnedGatt[2] = c2Cccd;
}

const char * UcxClientManager::GetMacSuffix()
{
    return ucxmatter::GetDeviceConfig().macSuffix.c_str();
}

} // namespace DeviceLayer
} // namespace chip
