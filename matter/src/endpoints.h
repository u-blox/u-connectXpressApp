/* The endpoints of the node. A kind is one row of the table in endpoints.cpp: adding a device type to the plug-in
 * is a new row, and which ones a node has is chosen when it starts (matter.start light temp). */
#ifndef UCXM_ENDPOINTS_H
#define UCXM_ENDPOINTS_H

#include <cstddef>
#include <cstdint>

namespace ucxm {

constexpr int kMaxEndpoints = 8;

struct Endpoint;

struct EndpointKind
{
    const char * name;     // matter.start <name>
    uint32_t deviceType;   // Matter device type
    const char * title;
    const char * values;   // what matter.set takes
    void (*init)(Endpoint & endpoint);
    bool (*set)(Endpoint & endpoint, int argc, const char * const argv[]);
    void (*show)(const Endpoint & endpoint, char * out, size_t size);
    uint16_t modelEndpoint; // the endpoint of this kind in data_model/ucxmatter.zap, for the first of the kind
};

struct Endpoint
{
    const EndpointKind * kind;
    uint16_t id;      // endpoint number on the node; 0 is the root node
    char name[16];    // the kind, numbered from the second of a kind: light, light2
    int32_t value[2]; // meaning is the kind's
};

int KindCount();
const EndpointKind & KindAt(int index);
const EndpointKind * KindFind(const char * name);

void EndpointsClear();
Endpoint * EndpointAdd(const EndpointKind & kind); // nullptr when the node is full
int EndpointCount();
Endpoint & EndpointAt(int index);
Endpoint * EndpointFind(const char * nameOrId);
// The data model has one endpoint of each kind: the first endpoint of a kind is tied to it, the others are the
// node's own (0)
uint16_t EndpointModel(const Endpoint & endpoint);
Endpoint * EndpointOfModel(uint16_t modelEndpoint);

} // namespace ucxm

#endif // UCXM_ENDPOINTS_H
