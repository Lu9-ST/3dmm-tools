// Kauai chunky file reader (.3cn) including KCD2-packed chunks.  The KCD2 decoder is a port of the portable decoder in
// 3DMMEx's kauai/src/codkauai.cpp (MIT licence, Copyright (c) Microsoft Corporation).
#ifndef DMM_CHUNKREAD_H
#define DMM_CHUNKREAD_H

#include "chunky.h"

namespace dmm
{

struct RChunk
{
    uint32_t ctg = 0, cno = 0;
    uint32_t fp = 0, size = 0;
    uint8_t flags = 0; // 4 = packed
    bool hasName = false;
    std::string name;
    std::vector<Kid> kids;
};

class ChunkFile
{
  public:
    bool Open(const std::string &path, std::string *err);
    const RChunk *Find(const char *ctg, uint32_t cno) const;
    const RChunk *Find(uint32_t ctg, uint32_t cno) const;
    // chunk data, unpacked when the chunk is packed
    bool Data(const RChunk *c, Bytes *out, std::string *err) const;
    // kid lookup: first kid of this chid and type
    bool KidCno(const RChunk *c, const char *ctg, uint32_t chid, uint32_t *cno) const;
    const std::vector<RChunk> &Chunks() const
    {
        return _chunks;
    }

  private:
    Bytes _file;
    std::vector<RChunk> _chunks;
};

// KCD2 stream (without the 8-byte codec header) -> bytes
bool Kcd2Decode(const uint8_t *src, size_t cbSrc, uint8_t *dst, size_t cbDst, size_t *cbOut);

// GL / GG containers (little-endian, as stored in the stock files)
struct GlView
{
    uint32_t cbEntry = 0;
    std::vector<Bytes> entries;
};
struct GgView
{
    uint32_t cbFixed = 0;
    std::vector<std::pair<Bytes, Bytes>> entries; // fixed part, variable part
};
bool ParseGl(const Bytes &d, GlView *out);
bool ParseGg(const Bytes &d, GgView *out);

} // namespace dmm

#endif
