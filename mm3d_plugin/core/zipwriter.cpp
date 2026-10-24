#include "zipwriter.h"

#ifdef DMM_USE_ZLIB
#include <zlib.h>
#endif

namespace dmm
{

uint32_t Crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

#ifdef DMM_USE_ZLIB
static bool Deflate(const Bytes &in, Bytes *out)
{
    z_stream zs = {};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    out->resize(deflateBound(&zs, (uLong)in.size()) + 16);
    zs.next_in = const_cast<Bytef *>(in.data());
    zs.avail_in = (uInt)in.size();
    zs.next_out = out->data();
    zs.avail_out = (uInt)out->size();
    int r = deflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END)
    {
        deflateEnd(&zs);
        return false;
    }
    out->resize(zs.total_out);
    deflateEnd(&zs);
    return true;
}
#endif

Bytes ZipWrite(const std::vector<std::pair<std::string, Bytes>> &entries)
{
    Bytes out, central;
    for (const auto &e : entries)
    {
        const Bytes &raw = e.second;
        uint32_t crc = Crc32(raw.data(), raw.size());
        Bytes packed;
        uint16_t method = 0;
#ifdef DMM_USE_ZLIB
        if (!raw.empty() && Deflate(raw, &packed) && packed.size() < raw.size())
            method = 8;
#endif
        const Bytes &body = (method == 8) ? packed : raw;
        uint32_t off = (uint32_t)out.size();

        Put32(out, 0x04034b50);
        Put16(out, 20); // version needed
        Put16(out, 0);  // flags
        Put16(out, method);
        Put16(out, 0);          // dos time
        Put16(out, 0x21);       // dos date (1980-01-01)
        Put32(out, crc);
        Put32(out, (uint32_t)body.size());
        Put32(out, (uint32_t)raw.size());
        Put16(out, (uint32_t)e.first.size());
        Put16(out, 0);
        for (char c : e.first)
            Put8(out, (uint8_t)c);
        PutBytes(out, body);

        Put32(central, 0x02014b50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, 0);
        Put16(central, method);
        Put16(central, 0);
        Put16(central, 0x21);
        Put32(central, crc);
        Put32(central, (uint32_t)body.size());
        Put32(central, (uint32_t)raw.size());
        Put16(central, (uint32_t)e.first.size());
        Put16(central, 0); // extra
        Put16(central, 0); // comment
        Put16(central, 0); // disk
        Put16(central, 0); // internal attrs
        Put32(central, 0); // external attrs
        Put32(central, off);
        for (char c : e.first)
            Put8(central, (uint8_t)c);
    }
    uint32_t cdOff = (uint32_t)out.size();
    PutBytes(out, central);
    Put32(out, 0x06054b50);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, (uint32_t)entries.size());
    Put16(out, (uint32_t)entries.size());
    Put32(out, (uint32_t)central.size());
    Put32(out, cdOff);
    Put16(out, 0);
    return out;
}

} // namespace dmm
