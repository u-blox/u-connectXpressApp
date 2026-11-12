#include "endpoints.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ucxm {
namespace {

// light: value[0] on/off, value[1] level in percent
void LightInit(Endpoint & e)
{
    e.value[0] = 0;
    e.value[1] = 100;
}

bool LightSet(Endpoint & e, int argc, const char * const argv[])
{
    char * end = nullptr;
    long level;

    if (argc != 1)
    {
        return false;
    }
    if (strcmp(argv[0], "on") == 0 || strcmp(argv[0], "off") == 0)
    {
        e.value[0] = argv[0][1] == 'n';
        return true;
    }
    level = strtol(argv[0], &end, 10);
    if (end == argv[0] || *end != '\0' || level < 0 || level > 100)
    {
        return false;
    }
    e.value[0] = level > 0;
    if (level > 0)
    {
        e.value[1] = static_cast<int32_t>(level);
    }
    return true;
}

void LightShow(const Endpoint & e, char * out, size_t size)
{
    if (e.value[0] != 0)
    {
        snprintf(out, size, "on %d%%", static_cast<int>(e.value[1]));
    }
    else
    {
        snprintf(out, size, "off");
    }
}

// temp: value[0] in 0.01 degrees Celsius, as the Temperature Measurement cluster counts
void TempInit(Endpoint & e)
{
    e.value[0] = 2150;
}

bool TempSet(Endpoint & e, int argc, const char * const argv[])
{
    char * end = nullptr;
    double degrees;

    if (argc != 1)
    {
        return false;
    }
    degrees = strtod(argv[0], &end);
    if (end == argv[0] || *end != '\0' || !(degrees >= -40.0 && degrees <= 125.0))
    {
        return false;
    }
    e.value[0] = static_cast<int32_t>(lround(degrees * 100.0));
    return true;
}

void TempShow(const Endpoint & e, char * out, size_t size)
{
    snprintf(out, size, "%.1f C", e.value[0] / 100.0);
}

// contact: value[0] 1 when open
void ContactInit(Endpoint & e)
{
    e.value[0] = 0;
}

bool ContactSet(Endpoint & e, int argc, const char * const argv[])
{
    if (argc != 1 || (strcmp(argv[0], "open") != 0 && strcmp(argv[0], "closed") != 0))
    {
        return false;
    }
    e.value[0] = argv[0][0] == 'o';
    return true;
}

void ContactShow(const Endpoint & e, char * out, size_t size)
{
    snprintf(out, size, "%s", e.value[0] != 0 ? "open" : "closed");
}

const EndpointKind kKinds[] = {
    { "light", 0x0101, "Dimmable Light", "on | off | 0..100 (percent)", LightInit, LightSet, LightShow },
    { "temp", 0x0302, "Temperature Sensor", "-40..125 (degrees Celsius)", TempInit, TempSet, TempShow },
    { "contact", 0x0015, "Contact Sensor", "open | closed", ContactInit, ContactSet, ContactShow },
};

Endpoint gEndpoints[kMaxEndpoints];
int gCount;

} // namespace

int KindCount()
{
    return static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));
}

const EndpointKind & KindAt(int index)
{
    return kKinds[index];
}

const EndpointKind * KindFind(const char * name)
{
    for (const EndpointKind & kind : kKinds)
    {
        if (strcmp(name, kind.name) == 0)
        {
            return &kind;
        }
    }
    return nullptr;
}

void EndpointsClear()
{
    gCount = 0;
}

Endpoint * EndpointAdd(const EndpointKind & kind)
{
    int same = 1;

    if (gCount >= kMaxEndpoints)
    {
        return nullptr;
    }
    for (int i = 0; i < gCount; i++)
    {
        same += gEndpoints[i].kind == &kind ? 1 : 0;
    }
    Endpoint & e = gEndpoints[gCount];
    e.kind       = &kind;
    e.id         = static_cast<uint16_t>(gCount + 1);
    if (same > 1)
    {
        snprintf(e.name, sizeof(e.name), "%s%d", kind.name, same);
    }
    else
    {
        snprintf(e.name, sizeof(e.name), "%s", kind.name);
    }
    kind.init(e);
    gCount++;
    return &e;
}

int EndpointCount()
{
    return gCount;
}

Endpoint & EndpointAt(int index)
{
    return gEndpoints[index];
}

Endpoint * EndpointFind(const char * nameOrId)
{
    char * end = nullptr;
    long id    = strtol(nameOrId, &end, 10);
    bool isId  = end != nameOrId && *end == '\0';

    for (int i = 0; i < gCount; i++)
    {
        if (isId ? gEndpoints[i].id == id : strcmp(gEndpoints[i].name, nameOrId) == 0)
        {
            return &gEndpoints[i];
        }
    }
    return nullptr;
}

} // namespace ucxm
