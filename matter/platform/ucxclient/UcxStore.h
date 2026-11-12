/* What the node keeps between runs, as named blobs in the app's secure store (app/src/ucxapp_matter.h): the app
 * seals them on disk. u-connectMatter wrote the same maps to plain files. */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ucxmatter {

using BlobMap = std::map<std::string, std::vector<uint8_t>>;

// name: a to z and 0 to 9, at most 16 characters
bool StoreLoad(const char * name, BlobMap & map); // false when nothing is stored yet; the map is then empty
bool StoreSave(const char * name, const BlobMap & map);
void StoreRemove(const char * name);

} // namespace ucxmatter
