#include "chunkread.h"

#include <cstdio>
#include <cstring>

namespace dmm
{

static uint32_t Rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t Rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool Kcd2Decode(const uint8_t *src, size_t cbSrc, uint8_t *dst, size_t cbDst, size_t *cbOut)
{
    const int kcbTail = 6, kcbitMaxLen = 11;
    const int kcbit[4] = {6, 9, 12, 20};
    const uint32_t kdib[4] = {0x01, 0x41, 0x0241, 0x1241};
    if (cbSrc <= (size_t)kcbTail + 1 || src[0] != 0)
        return false;
    for (int i = 1; i <= kcbTail; i++)
        if (src[cbSrc - i] != 0xFF)
            return false;
    const uint8_t *pbSrc = src + 1;
    const uint8_t *pbLimSrc = src + cbSrc - kcbTail;
    uint8_t *pbDst = dst, *pbLimDst = dst + cbDst;
    uint32_t luCur = 0;
    auto adv = [&](int cb) {
        pbSrc += cb;
        luCur = Rd32(pbSrc - 4);
    };
    adv(4);
    int ibit = 0;
    for (;;)
    {
        int cbit = 0;
        for (;;)
        {
            if (!(luCur & (1u << (ibit + cbit))))
                break;
            if (++cbit > kcbitMaxLen)
                goto done;
        }
        {
            int32_t cb = ((1 << cbit) - 1) + (int32_t)((luCur >> (ibit + cbit + 1)) & ((1u << cbit) - 1));
            ibit += cbit + cbit + 1;
            adv(ibit >> 3);
            ibit &= 7;
            if (!(luCur & (1u << ibit)))
            {
                ibit++;
                bool aligned = (ibit == 8);
                uint8_t bT = 0;
                int cbitHi = 0;
                if (aligned)
                    cb++;
                else
                {
                    bT = (uint8_t)(((luCur & ~((1u << ibit) - 1)) & 0xFF) >> ibit);
                    cbitHi = ibit;
                    ibit += (8 - ibit);
                }
                if (pbDst + cb > pbLimDst || pbSrc - 3 + cb > pbLimSrc + 0 + 4)
                    return false;
                memcpy(pbDst, pbSrc - 3, (size_t)cb);
                pbDst += cb;
                adv(cb);
                if (!aligned)
                {
                    uint8_t bUpper = (uint8_t)((luCur & ((1u << (ibit + cbitHi)) - 1)) >> ibit);
                    bT |= (uint8_t)(bUpper << (8 - cbitHi));
                    if (pbDst >= pbLimDst)
                        return false;
                    *pbDst++ = bT;
                    ibit += cbitHi;
                }
            }
            else
            {
                cb += 2;
                uint32_t dib;
                if (!(luCur & (1u << (ibit + 1))))
                {
                    dib = ((luCur >> (ibit + 2)) & ((1u << kcbit[0]) - 1)) + kdib[0];
                    ibit += 2 + kcbit[0];
                }
                else if (!(luCur & (1u << (ibit + 2))))
                {
                    dib = ((luCur >> (ibit + 3)) & ((1u << kcbit[1]) - 1)) + kdib[1];
                    ibit += 3 + kcbit[1];
                }
                else if (!(luCur & (1u << (ibit + 3))))
                {
                    dib = ((luCur >> (ibit + 4)) & ((1u << kcbit[2]) - 1)) + kdib[2];
                    ibit += 4 + kcbit[2];
                }
                else
                {
                    dib = ((luCur >> (ibit + 4)) & ((1u << kcbit[3]) - 1)) + kdib[3];
                    ibit += 4 + kcbit[3];
                    cb++;
                }
                if ((size_t)(pbLimDst - pbDst) < (size_t)cb || (size_t)(pbDst - dst) < dib)
                    return false;
                const uint8_t *pbT = pbDst - dib;
                while (cb-- > 0)
                    *pbDst++ = *pbT++;
            }
            adv(ibit >> 3);
            ibit &= 7;
        }
    }
done:
    *cbOut = (size_t)(pbDst - dst);
    return true;
}

bool ChunkFile::Open(const std::string &path, std::string *err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
    {
        if (err)
            *err = "cannot open " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    _file.resize((size_t)n);
    size_t got = n > 0 ? fread(_file.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n || n < 128 || memcmp(_file.data(), "CHN2", 4) != 0)
    {
        if (err)
            *err = path + " is not a Kauai chunky file";
        return false;
    }
    const uint8_t *h = _file.data();
    uint16_t bo = Rd16(h + 12);
    uint32_t fpIdx = Rd32(h + 20), cbIdx = Rd32(h + 24);
    if (bo != 1 || (uint64_t)fpIdx + cbIdx > _file.size() || cbIdx < 20)
    {
        if (err)
            *err = path + ": unsupported chunky header";
        return false;
    }
    const uint8_t *g = h + fpIdx;
    uint32_t iloc = Rd32(g + 4), bvMac = Rd32(g + 8), cbFixed = Rd32(g + 16);
    if (cbFixed != 20 || 20 + (uint64_t)bvMac + 8ull * iloc > cbIdx)
    {
        if (err)
            *err = path + ": bad chunk index";
        return false;
    }
    for (uint32_t i = 0; i < iloc; i++)
    {
        uint32_t bv = Rd32(g + 20 + bvMac + 8 * i), cb = Rd32(g + 24 + bvMac + 8 * i);
        if ((int32_t)cb < 20 || (uint64_t)bv + cb > bvMac)
            continue; // free slot
        const uint8_t *e = g + 20 + bv;
        RChunk c;
        c.ctg = Rd32(e);
        c.cno = Rd32(e + 4);
        c.fp = Rd32(e + 8);
        uint32_t lu = Rd32(e + 12);
        c.flags = (uint8_t)(lu & 0xff);
        c.size = lu >> 8;
        uint32_t ckid = Rd16(e + 16);
        if (20 + 12ull * ckid > cb)
            continue;
        for (uint32_t k = 0; k < ckid; k++)
            c.kids.push_back(Kid{Rd32(e + 20 + 12 * k + 8), Rd32(e + 20 + 12 * k), Rd32(e + 20 + 12 * k + 4)});
        size_t rest = 20 + 12ull * ckid;
        if (rest + 3 <= cb)
        {
            size_t len = e[rest + 2];
            if (rest + 3 + len <= cb)
            {
                c.hasName = true;
                c.name.assign((const char *)e + rest + 3, len);
            }
        }
        _chunks.push_back(c);
    }
    return true;
}

const RChunk *ChunkFile::Find(uint32_t ctg, uint32_t cno) const
{
    for (const RChunk &c : _chunks)
        if (c.ctg == ctg && c.cno == cno)
            return &c;
    return nullptr;
}
const RChunk *ChunkFile::Find(const char *ctg, uint32_t cno) const
{
    return Find(Ctg(ctg), cno);
}

bool ChunkFile::KidCno(const RChunk *c, const char *ctg, uint32_t chid, uint32_t *cno) const
{
    uint32_t t = Ctg(ctg);
    for (const dmm::Kid &k : c->kids)
        if (k.ctg == t && k.chid == chid)
        {
            *cno = k.cno;
            return true;
        }
    return false;
}

bool ChunkFile::Data(const RChunk *c, Bytes *out, std::string *err) const
{
    if ((uint64_t)c->fp + c->size > _file.size())
    {
        if (err)
            *err = "chunk data out of range";
        return false;
    }
    const uint8_t *p = _file.data() + c->fp;
    if (!(c->flags & 4))
    {
        out->assign(p, p + c->size);
        return true;
    }
    if (c->size < 8 + 8)
    {
        if (err)
            *err = "packed chunk too small";
        return false;
    }
    // codec header: format (big-endian 4cc) + unpacked size (big-endian)
    uint32_t cbDst = ((uint32_t)p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    if (memcmp(p, "KCD2", 4) != 0 || cbDst > (64u << 20))
    {
        if (err)
            *err = "unsupported chunk compression";
        return false;
    }
    out->assign(cbDst + 8, 0);
    size_t got = 0;
    if (!Kcd2Decode(p + 8, c->size - 8, out->data(), cbDst, &got) || got != cbDst)
    {
        if (err)
            *err = "KCD2 decode failed";
        return false;
    }
    out->resize(cbDst);
    return true;
}

bool ParseGl(const Bytes &d, GlView *out)
{
    if (d.size() < 12)
        return false;
    out->cbEntry = Rd32(d.data() + 4);
    uint32_t n = Rd32(d.data() + 8);
    if (12 + (uint64_t)out->cbEntry * n > d.size())
        return false;
    for (uint32_t i = 0; i < n; i++)
        out->entries.push_back(Bytes(d.begin() + 12 + (size_t)i * out->cbEntry, d.begin() + 12 + (size_t)(i + 1) * out->cbEntry));
    return true;
}

bool ParseGg(const Bytes &d, GgView *out)
{
    if (d.size() < 20)
        return false;
    uint32_t iloc = Rd32(d.data() + 4), bvMac = Rd32(d.data() + 8);
    out->cbFixed = Rd32(d.data() + 16);
    if (20 + (uint64_t)bvMac + 8ull * iloc > d.size())
        return false;
    for (uint32_t i = 0; i < iloc; i++)
    {
        uint32_t bv = Rd32(d.data() + 20 + bvMac + 8 * i), cb = Rd32(d.data() + 24 + bvMac + 8 * i);
        if ((int32_t)cb < (int32_t)out->cbFixed || (uint64_t)bv + cb > bvMac)
            return false;
        const uint8_t *e = d.data() + 20 + bv;
        out->entries.push_back(std::make_pair(Bytes(e, e + out->cbFixed), Bytes(e + out->cbFixed, e + cb)));
    }
    return true;
}

} // namespace dmm
