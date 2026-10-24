// Kauai chunky file (.3cn/.3th) writer.  Little-endian, uncompressed.
// Format notes: see tools/chunky.py (the Python prototype this is a port of).
#ifndef DMM_CHUNKY_H
#define DMM_CHUNKY_H

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace dmm
{

typedef std::vector<uint8_t> Bytes;

// 'BKGD' -> uint32 as Kauai stores it (big-endian-packed char constant)
uint32_t Ctg(const char *s);

struct Kid
{
    uint32_t chid;
    uint32_t ctg;
    uint32_t cno;
    bool operator<(const Kid &o) const
    {
        return std::tie(chid, ctg, cno) < std::tie(o.chid, o.ctg, o.cno);
    }
    bool operator==(const Kid &o) const
    {
        return chid == o.chid && ctg == o.ctg && cno == o.cno;
    }
};

struct Chunk
{
    uint32_t ctg = 0;
    uint32_t cno = 0;
    Bytes data;
    bool hasName = false;
    std::string name;
    std::vector<Kid> kids;

    void AddKid(uint32_t chid, const char *ctgName, uint32_t cnoKid)
    {
        kids.push_back(Kid{chid, Ctg(ctgName), cnoKid});
    }
};

class Chunky
{
  public:
    explicit Chunky(const char *creator = "CHMP") : _creator(Ctg(creator))
    {
    }

    // Adds (or replaces) a chunk and returns a stable pointer to it.
    Chunk *Add(const char *ctgName, uint32_t cno, const Bytes &data, const char *name = nullptr);

    // Serialises to a Kauai chunky file image.  Returns false (and sets *err) when a kid points at a missing chunk.
    bool Write(Bytes *out, std::string *err) const;

  private:
    uint32_t _creator;
    std::map<std::pair<uint32_t, uint32_t>, Chunk> _chunks;
};

// little-endian append helpers shared by the encoders
inline void Put8(Bytes &b, uint32_t v)
{
    b.push_back((uint8_t)v);
}
inline void Put16(Bytes &b, uint32_t v)
{
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
}
inline void Put32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b.push_back((uint8_t)(v >> (8 * i)));
}
inline void PutZeros(Bytes &b, size_t n)
{
    b.insert(b.end(), n, 0);
}
inline void PutBytes(Bytes &b, const Bytes &o)
{
    b.insert(b.end(), o.begin(), o.end());
}

} // namespace dmm

#endif
