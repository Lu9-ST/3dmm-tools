#include "chunky.h"

#include <algorithm>

namespace dmm
{

uint32_t Ctg(const char *s)
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) |
           (uint32_t)(uint8_t)s[3];
}

Chunk *Chunky::Add(const char *ctgName, uint32_t cno, const Bytes &data, const char *name)
{
    Chunk c;
    c.ctg = Ctg(ctgName);
    c.cno = cno;
    c.data = data;
    if (name != nullptr)
    {
        c.hasName = true;
        c.name = name;
    }
    Chunk &slot = _chunks[std::make_pair(c.ctg, c.cno)];
    slot = c;
    return &slot; // std::map nodes are stable
}

// STN::GetData: osk(int16), length byte, chars, NUL
static Bytes StnData(const std::string &s)
{
    Bytes b;
    Put16(b, 0x0303);
    Put8(b, (uint32_t)std::min<size_t>(s.size(), 255));
    for (size_t i = 0; i < s.size() && i < 255; i++)
        Put8(b, (uint8_t)s[i]);
    Put8(b, 0);
    return b;
}

bool Chunky::Write(Bytes *out, std::string *err) const
{
    // reference counts (ccrpRef must equal the number of parents)
    std::map<std::pair<uint32_t, uint32_t>, int> refs;
    for (const auto &kv : _chunks)
        refs[kv.first] = 0;
    for (const auto &kv : _chunks)
    {
        for (const Kid &k : kv.second.kids)
        {
            auto it = refs.find(std::make_pair(k.ctg, k.cno));
            if (it == refs.end())
            {
                if (err)
                    *err = "chunk has a kid that does not exist";
                return false;
            }
            it->second++;
        }
    }

    Bytes heap(128, 0);
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> placed;
    for (const auto &kv : _chunks) // std::map iterates in (ctg, cno) order, same as the reader expects
    {
        placed[kv.first] = (uint32_t)heap.size();
        PutBytes(heap, kv.second.data);
    }

    Bytes idx;
    std::vector<std::pair<uint32_t, uint32_t>> locs;
    for (const auto &kv : _chunks)
    {
        const Chunk &ch = kv.second;
        std::vector<Kid> kids = ch.kids;
        std::sort(kids.begin(), kids.end());
        for (size_t i = 1; i < kids.size(); i++)
        {
            if (kids[i] == kids[i - 1])
            {
                if (err)
                    *err = "duplicate kid";
                return false;
            }
        }
        int r = refs[kv.first];
        uint32_t fl = (r == 0) ? 2 : 0; // unreferenced chunks must carry the Loner flag
        size_t start = idx.size();
        Put32(idx, ch.ctg);
        Put32(idx, ch.cno);
        Put32(idx, placed[kv.first]);
        Put32(idx, ((uint32_t)ch.data.size() << 8) | fl);
        Put16(idx, (uint32_t)kids.size());
        Put16(idx, (uint32_t)r);
        for (const Kid &k : kids)
        {
            Put32(idx, k.ctg);
            Put32(idx, k.cno);
            Put32(idx, k.chid);
        }
        if (ch.hasName)
            PutBytes(idx, StnData(ch.name));
        locs.push_back(std::make_pair((uint32_t)start, (uint32_t)(idx.size() - start)));
    }

    Bytes g;
    Put16(g, 1);
    Put16(g, 0x0303);
    Put32(g, (uint32_t)locs.size());
    Put32(g, (uint32_t)idx.size());
    Put32(g, (uint32_t)-1);
    Put32(g, 20);
    PutBytes(g, idx);
    for (const auto &l : locs)
    {
        Put32(g, l.first);
        Put32(g, l.second);
    }

    uint32_t fpIdx = (uint32_t)heap.size();
    Bytes hdr;
    hdr.push_back('C');
    hdr.push_back('H');
    hdr.push_back('N');
    hdr.push_back('2');
    Put32(hdr, _creator);
    Put16(hdr, 5);
    Put16(hdr, 4);
    Put16(hdr, 1);
    Put16(hdr, 0x0303);
    Put32(hdr, fpIdx + (uint32_t)g.size()); // fpMac
    Put32(hdr, fpIdx);                      // fpIndex
    Put32(hdr, (uint32_t)g.size());         // cbIndex
    Put32(hdr, fpIdx + (uint32_t)g.size()); // fpMap
    Put32(hdr, 0);                          // cbMap
    hdr.resize(128, 0);
    std::copy(hdr.begin(), hdr.end(), heap.begin());

    out->clear();
    PutBytes(*out, heap);
    PutBytes(*out, g);
    return true;
}

} // namespace dmm
