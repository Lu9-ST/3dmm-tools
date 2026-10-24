// Minimal zip writer (stored entries; deflate when zlib is available).
#ifndef DMM_ZIPWRITER_H
#define DMM_ZIPWRITER_H

#include "chunky.h"

#include <string>
#include <utility>
#include <vector>

namespace dmm
{

uint32_t Crc32(const uint8_t *p, size_t n);

// entries: (name, data).  Output is a standard zip archive.
Bytes ZipWrite(const std::vector<std::pair<std::string, Bytes>> &entries);

} // namespace dmm

#endif
