#include "actor.h"

#include "zipwriter.h"

#include <cmath>
#include <cstring>
#include <set>

namespace dmm
{

static const int16_t BO = 1;
static const int16_t OSK = 0x0303;

static int32_t Fx(double v)
{
    return (int32_t)std::lround(v * 65536.0);
}

// ---------------------------------------------------------------- primitive encoders

static Bytes EncGl(uint32_t cbEntry, const std::vector<Bytes> &entries)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put32(b, cbEntry);
    Put32(b, (uint32_t)entries.size());
    for (const Bytes &e : entries)
        PutBytes(b, e);
    return b;
}

static Bytes EncGl16(const std::vector<int> &vals)
{
    std::vector<Bytes> e;
    for (int v : vals)
    {
        Bytes x;
        Put16(x, (uint16_t)(int16_t)v);
        e.push_back(x);
    }
    return EncGl(2, e);
}

struct GgEntry
{
    Bytes fixed, var;
};

static Bytes EncGg(uint32_t cbFixed, const std::vector<GgEntry> &entries)
{
    Bytes data;
    std::vector<std::pair<uint32_t, uint32_t>> locs;
    for (const GgEntry &e : entries)
    {
        locs.push_back(std::make_pair((uint32_t)data.size(), (uint32_t)(e.fixed.size() + e.var.size())));
        PutBytes(data, e.fixed);
        PutBytes(data, e.var);
    }
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put32(b, (uint32_t)entries.size());
    Put32(b, (uint32_t)data.size());
    Put32(b, (uint32_t)-1);
    Put32(b, cbFixed);
    PutBytes(b, data);
    for (const auto &l : locs)
    {
        Put32(b, l.first);
        Put32(b, l.second);
    }
    return b;
}

Bytes EncBmdl(const Mesh &m)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put16(b, (uint32_t)m.verts.size());
    Put16(b, (uint32_t)m.faces.size());
    PutZeros(b, 40); // MODLF header: radius etc. are zero => "unprepared", BRender prepares it on load
    for (const Vertex &v : m.verts)
    {
        Put32(b, (uint32_t)Fx(v.x));
        Put32(b, (uint32_t)Fx(v.y));
        Put32(b, (uint32_t)Fx(v.z));
        Put32(b, (uint32_t)(int32_t)std::lround(v.u * 65535.0));
        Put32(b, (uint32_t)(int32_t)std::lround(v.v * 65535.0));
        PutZeros(b, 12);
    }
    for (const auto &f : m.faces)
    {
        Put16(b, f[0]);
        Put16(b, f[1]);
        Put16(b, f[2]);
        PutZeros(b, 6); // edges
        PutZeros(b, 4); // material
        Put16(b, 1);    // smoothing
        PutZeros(b, 14);
    }
    return b;
}

Bytes EncTmap(const Image8 &img)
{
    Bytes b;
    b.push_back(1);
    b.push_back(0);
    b.push_back(3);
    b.push_back(3);
    Put16(b, (uint32_t)img.w);
    b.push_back(3);
    b.push_back(2);
    Put32(b, 0);
    Put16(b, (uint32_t)img.w);
    Put16(b, (uint32_t)img.h);
    Put32(b, 0);
    for (int y = img.h - 1; y >= 0; y--) // rows are stored bottom-up
        b.insert(b.end(), img.px.begin() + (size_t)y * img.w, img.px.begin() + (size_t)(y + 1) * img.w);
    return b;
}

Bytes EncMbmp(const Image8 &img)
{
    Bytes body;
    std::vector<Bytes> rows;
    for (int y = 0; y < img.h; y++)
    {
        // every pixel is opaque: runs of up to 255 pixels, no skips
        Bytes r;
        int x = 0;
        while (x < img.w)
        {
            int n = std::min(255, img.w - x);
            r.push_back(0);
            r.push_back((uint8_t)n);
            r.insert(r.end(), img.px.begin() + (size_t)y * img.w + x, img.px.begin() + (size_t)y * img.w + x + n);
            x += n;
        }
        rows.push_back(r);
    }
    for (const Bytes &r : rows)
        Put16(body, (uint32_t)r.size());
    for (const Bytes &r : rows)
        PutBytes(body, r);
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put8(b, 0);
    Put8(b, 0);
    Put16(b, 0);
    Put32(b, 0);
    Put32(b, 0);
    Put32(b, (uint32_t)img.w);
    Put32(b, (uint32_t)img.h);
    Put32(b, (uint32_t)(28 + body.size()));
    PutBytes(b, body);
    return b;
}

static Bytes EncMtrl(int base, int rng)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put32(b, 0);     // brc
    Put16(b, 0);     // Ka
    Put16(b, 65535); // Kd
    Put16(b, 0);     // Ks
    Put8(b, (uint32_t)base);
    Put8(b, (uint32_t)rng);
    Put32(b, 3276800); // power 50.0
    return b;
}

static Bytes EncCmtl(int ibset)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put32(b, (uint32_t)ibset);
    return b;
}

static Bytes EncTmpl(const int16_t rest[3], bool prop)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    for (int i = 0; i < 3; i++)
        Put16(b, (uint16_t)rest[i]);
    Put16(b, 0x0014);
    Put32(b, prop ? 4 : 1);
    return b;
}

static Bytes EncActn(uint32_t flags)
{
    Bytes b;
    Put16(b, BO);
    Put16(b, OSK);
    Put32(b, flags);
    return b;
}

static Bytes EncMatrix(const Mat34 &m)
{
    Bytes b;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            Put32(b, (uint32_t)Fx(m.r[i][j]));
    for (int j = 0; j < 3; j++)
        Put32(b, (uint32_t)Fx(m.t[j]));
    return b;
}

static const uint8_t kGokd[] = {0x01, 0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 0xc3, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};

// ---------------------------------------------------------------- the actor

namespace
{
struct Counters
{
    uint32_t base;
    std::map<std::string, uint32_t> next;
    uint32_t Nxt(const char *ctg)
    {
        auto it = next.find(ctg);
        uint32_t n = (it == next.end()) ? base : it->second;
        next[ctg] = n + 1;
        return n;
    }
};
} // namespace

bool BuildActor(const Spec &spec, uint32_t cnoBase, BuildResult *out, std::string *err)
{
    auto fail = [&](const char *msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (spec.parts.empty())
        return fail("actor has no body parts");
    if (spec.actions.empty())
        return fail("actor has no actions");
    if (spec.thumb.w != 72 || spec.thumb.h != 72 || spec.thumb.px.size() != 72 * 72)
        return fail("thumbnail must be 72x72");

    const size_t nparts = spec.parts.size();
    int nsets = 0;
    for (const Part &p : spec.parts)
    {
        if (p.bset < 0 || p.parent >= (int)nparts)
            return fail("bad part");
        nsets = std::max(nsets, p.bset + 1);
    }
    if ((int)spec.costumes.size() != nsets)
        return fail("costume list does not match the body part sets");
    for (int s = 0; s < nsets; s++)
        if (spec.costumes[s].empty())
            return fail("a body part set has no costumes");

    Chunky sc, th;
    Counters cnt;
    cnt.base = cnoBase;

    Chunk *tm = sc.Add("TMPL", cnoBase, EncTmpl(spec.rest, spec.prop), spec.name.c_str());

    {
        std::vector<int> par, bs;
        for (const Part &p : spec.parts)
        {
            par.push_back(p.parent);
            bs.push_back(p.bset);
        }
        uint32_t c = cnt.Nxt("GLPI");
        sc.Add("GLPI", c, EncGl16(par));
        tm->AddKid(0, "GLPI", c);
        c = cnt.Nxt("GLBS");
        sc.Add("GLBS", c, EncGl16(bs));
        tm->AddKid(0, "GLBS", c);
    }

    // accessory models hang off the CMTLs (not the TMPL); costumes that use the same model share one chunk
    std::vector<uint32_t> accCno;
    for (const MeshPtr &m : spec.accModels)
    {
        uint32_t c = cnt.Nxt("BMDL");
        sc.Add("BMDL", c, EncBmdl(*m));
        accCno.push_back(c);
    }

    std::map<std::pair<const Image8 *, int>, uint32_t> mtrlCache;
    std::vector<GgEntry> ggcm;
    uint32_t cmid = 0;
    for (int s = 0; s < nsets; s++)
    {
        std::vector<int> members;
        for (size_t i = 0; i < nparts; i++)
            if (spec.parts[i].bset == s)
                members.push_back((int)i);
        std::vector<int32_t> ids;
        for (const auto &costume : spec.costumes[s])
        {
            uint32_t cmc = cnt.Nxt("CMTL");
            Chunk *cm = sc.Add("CMTL", cmc, EncCmtl(s));
            tm->AddKid(cmid, "CMTL", cmc);
            for (size_t k = 0; k < members.size(); k++)
            {
                Look look;
                auto it = costume.find(members[k]);
                if (it != costume.end())
                    look = it->second;
                std::pair<const Image8 *, int> key(look.texture.get(), look.texture ? 0 : look.solidBase);
                auto cit = mtrlCache.find(key);
                uint32_t mc;
                if (cit != mtrlCache.end())
                    mc = cit->second;
                else
                {
                    mc = cnt.Nxt("MTRL");
                    Chunk *mt = sc.Add("MTRL", mc, look.texture ? EncMtrl(15, 7) : EncMtrl(look.solidBase, 7));
                    if (look.texture)
                    {
                        uint32_t tc = cnt.Nxt("TMAP");
                        sc.Add("TMAP", tc, EncTmap(*look.texture));
                        mt->AddKid(0, "TMAP", tc);
                    }
                    mtrlCache[key] = mc;
                }
                cm->AddKid((uint32_t)k, "MTRL", mc);
                if (look.accModel >= 0)
                {
                    if (look.accModel >= (int)accCno.size())
                        return fail("accessory model index out of range");
                    cm->AddKid((uint32_t)k, "BMDL", accCno[look.accModel]);
                }
            }
            ids.push_back((int32_t)cmid);
            cmid++;
        }
        GgEntry e;
        Put32(e.fixed, (uint32_t)ids.size());
        for (int32_t id : ids)
            Put32(e.var, (uint32_t)id);
        ggcm.push_back(e);
    }
    {
        uint32_t c = cnt.Nxt("GGCM");
        sc.Add("GGCM", c, EncGg(4, ggcm));
        tm->AddKid(0, "GGCM", c);
    }

    for (size_t i = 0; i < spec.models.size(); i++)
    {
        uint32_t c = cnt.Nxt("BMDL");
        sc.Add("BMDL", c, EncBmdl(*spec.models[i]));
        tm->AddKid((uint32_t)i, "BMDL", c);
    }

    for (size_t ai = 0; ai < spec.actions.size(); ai++)
    {
        const Action &a = spec.actions[ai];
        uint32_t ac = cnt.Nxt("ACTN");
        Chunk *an = sc.Add("ACTN", ac, EncActn(a.flagsRaw >= 0 ? (uint32_t)a.flagsRaw : (a.moving ? 0x02 : 0x0a)), a.name.c_str()); // RotateY (+Static for stationary actions)
        tm->AddKid((uint32_t)ai, "ACTN", ac);

        std::vector<Bytes> mats;
        std::map<Bytes, int> mindex;
        auto matIdx = [&](const Mat34 &m) {
            Bytes d = EncMatrix(m);
            auto it = mindex.find(d);
            if (it != mindex.end())
                return it->second;
            int idx = (int)mats.size();
            mindex[d] = idx;
            mats.push_back(d);
            return idx;
        };
        std::vector<GgEntry> entries;
        for (const auto &cel : a.cels)
        {
            if (cel.size() != nparts)
                return fail("a cel does not list every body part");
            GgEntry e;
            const size_t celIdx = (size_t)(&cel - &a.cels[0]);
            if (celIdx < a.celFixed.size())
            {
                Put32(e.fixed, a.celFixed[celIdx][0]);
                Put32(e.fixed, a.celFixed[celIdx][1]);
            }
            else
            {
                Put32(e.fixed, 0);
                Put32(e.fixed, (uint32_t)Fx(a.dwr));
            }
            for (size_t pi = 0; pi < nparts; pi++)
            {
                int mdl = cel[pi].model;
                if (mdl >= (int)spec.models.size())
                    return fail("cel refers to a missing model");
                Put16(e.var, (uint16_t)(int16_t)mdl);
                Put16(e.var, (uint16_t)matIdx(cel[pi].m));
            }
            entries.push_back(e);
        }
        uint32_t gc = cnt.Nxt("GGCL");
        sc.Add("GGCL", gc, EncGg(8, entries));
        an->AddKid(0, "GGCL", gc);
        uint32_t xc = cnt.Nxt("GLXF");
        sc.Add("GLXF", xc, EncGl(48, mats));
        an->AddKid(0, "GLXF", xc);
    }

    // thumbnail file
    {
        Bytes tfc;
        Put16(tfc, BO);
        Put16(tfc, OSK);
        Put32(tfc, Ctg("TMPL"));
        Put32(tfc, cnoBase);
        Chunk *tt = th.Add(spec.prop ? "PRTH" : "TMTH", cnoBase, tfc);
        Chunk *g = th.Add("GOKD", cnoBase, Bytes(kGokd, kGokd + sizeof(kGokd)));
        tt->AddKid(0, "GOKD", cnoBase);
        th.Add("MBMP", cnoBase, EncMbmp(spec.thumb));
        g->AddKid(65536, "MBMP", cnoBase);
    }

    if (!sc.Write(&out->cn3, err) || !th.Write(&out->th3, err))
        return false;
    return true;
}

bool BuildVxp(const Spec &spec, const std::string &base, uint32_t cnoBase, const std::string &title,
              const std::string &author, Bytes *vxp, std::string *err)
{
    BuildResult r;
    if (!BuildActor(spec, cnoBase, &r, err))
        return false;
    std::string cfg = "Name=" + title + "\r\n";
    if (!author.empty())
        cfg += "Author=" + author + "\r\n";
    cfg += "Content=Actors\r\nGenerator=3DMM Actor Exporter for Maverick Model 3D\r\n";
    std::vector<std::pair<std::string, Bytes>> files;
    files.push_back(std::make_pair(base + ".3cn", r.cn3));
    files.push_back(std::make_pair(base + ".3th", r.th3));
    files.push_back(std::make_pair(base + ".cfg", Bytes(cfg.begin(), cfg.end())));
    *vxp = ZipWrite(files);
    return true;
}

} // namespace dmm
