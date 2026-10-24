#include <functional>
#include <cmath>
#include "stock.h"

#include "chunkread.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

namespace dmm
{

static int32_t S32(const uint8_t *p)
{
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

std::vector<Aff> StockActor::World(const StockCel &c) const
{
    std::vector<Aff> w(parent.size());
    for (size_t i = 0; i < parent.size(); i++)
    {
        Aff m = FromMat34(c.m[i]);
        // parents may come later in the list in principle; stock files list them first, but resolve generally
        int p = parent[i];
        if (p < 0)
            w[i] = m;
        else if ((size_t)p < i)
            w[i] = Mul(w[p], m);
        else
        {
            Aff acc = m;
            int q = p, guard = 0;
            while (q >= 0 && guard++ < 64)
            {
                acc = Mul(FromMat34(c.m[q]), acc);
                q = parent[q];
            }
            w[i] = acc;
        }
    }
    return w;
}

bool LoadStockActor(const std::string &path, const std::string &actorName, StockActor *out, std::string *err)
{
    ChunkFile f;
    if (!f.Open(path, err))
        return false;
    const RChunk *tm = nullptr;
    for (const RChunk &c : f.Chunks())
        if (c.ctg == Ctg("TMPL") && c.hasName && strcasecmp(c.name.c_str(), actorName.c_str()) == 0)
            tm = &c;
    if (!tm)
    {
        if (err)
            *err = "no actor called '" + actorName + "' in " + path;
        return false;
    }
    auto fail = [&](const std::string &m) {
        if (err)
            *err = m;
        return false;
    };
    Bytes d;
    if (!f.Data(tm, &d, err) || d.size() < 10)
        return fail("bad TMPL chunk");
    out->name = tm->name;
    for (int i = 0; i < 3; i++)
        out->rest[i] = (int16_t)(d[4 + 2 * i] | (d[5 + 2 * i] << 8));

    auto kidData = [&](const RChunk *c, const char *ctg, uint32_t chid, Bytes *b) {
        uint32_t cno;
        if (!f.KidCno(c, ctg, chid, &cno))
            return false;
        const RChunk *k = f.Find(ctg, cno);
        return k && f.Data(k, b, err);
    };
    Bytes glpi, glbs;
    GlView gp, gb;
    if (!kidData(tm, "GLPI", 0, &glpi) || !ParseGl(glpi, &gp) || !kidData(tm, "GLBS", 0, &glbs) || !ParseGl(glbs, &gb))
        return fail("TMPL has no part tree");
    size_t n = gp.entries.size();
    if (gb.entries.size() != n || gp.cbEntry != 2 || gb.cbEntry != 2)
        return fail("unexpected part list layout");
    for (size_t i = 0; i < n; i++)
    {
        out->parent.push_back((int16_t)(gp.entries[i][0] | (gp.entries[i][1] << 8)));
        out->bset.push_back((int16_t)(gb.entries[i][0] | (gb.entries[i][1] << 8)));
    }

    // actions: kids of the TMPL with ctg ACTN, in chid order
    std::vector<Kid> actKids;
    for (const Kid &k : tm->kids)
        if (k.ctg == Ctg("ACTN"))
            actKids.push_back(k);
    std::sort(actKids.begin(), actKids.end(), [](const Kid &a, const Kid &b) { return a.chid < b.chid; });
    for (const Kid &k : actKids)
    {
        const RChunk *ac = f.Find("ACTN", k.cno);
        if (!ac)
            continue;
        StockAction sa;
        sa.name = ac->hasName ? ac->name : "";
        Bytes ad;
        if (!f.Data(ac, &ad, err) || ad.size() < 8)
            return fail("bad ACTN chunk");
        sa.flags = (uint32_t)S32(ad.data() + 4);
        Bytes ggcl, glxf;
        GgView gg;
        GlView gx;
        if (!kidData(ac, "GGCL", 0, &ggcl) || !ParseGg(ggcl, &gg) || !kidData(ac, "GLXF", 0, &glxf) || !ParseGl(glxf, &gx))
            return fail("action '" + sa.name + "' has no cels");
        if (gg.cbFixed != 8 || gx.cbEntry != 48)
            return fail("unexpected action layout");
        for (const auto &e : gg.entries)
        {
            if (e.second.size() < 4 * n)
                return fail("short cel in action '" + sa.name + "'");
            StockCel c;
            c.fixed[0] = (uint32_t)S32(e.first.data());
            c.fixed[1] = (uint32_t)S32(e.first.data() + 4);
            for (size_t i = 0; i < n; i++)
            {
                const uint8_t *p = e.second.data() + 4 * i;
                int model = (int16_t)(p[0] | (p[1] << 8));
                int mi = (int16_t)(p[2] | (p[3] << 8));
                if (mi < 0 || (size_t)mi >= gx.entries.size())
                    return fail("bad matrix index in action '" + sa.name + "'");
                const Bytes &mb = gx.entries[(size_t)mi];
                Mat34 m;
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++)
                        m.r[a][b] = (float)(S32(mb.data() + 4 * (3 * a + b)) / 65536.0);
                for (int b = 0; b < 3; b++)
                    m.t[b] = (float)(S32(mb.data() + 36 + 4 * b) / 65536.0);
                c.model.push_back(model);
                c.m.push_back(m);
            }
            sa.cels.push_back(c);
        }
        out->actions.push_back(sa);
    }
    if (out->actions.empty())
        return fail("actor has no actions");

    // bounding box of the rest pose
    {
        std::vector<Aff> w = out->World(out->actions[0].cels[0]);
        bool first = true;
        for (size_t i = 0; i < n; i++)
        {
            int model = out->actions[0].cels[0].model[i];
            if (model < 0)
                continue;
            uint32_t cno;
            Bytes bd;
            if (!f.KidCno(tm, "BMDL", (uint32_t)model, &cno))
                continue;
            const RChunk *bc = f.Find("BMDL", cno);
            if (!bc || !f.Data(bc, &bd, err) || bd.size() < 48)
                continue;
            size_t nv = (size_t)(bd[4] | (bd[5] << 8));
            for (size_t v = 0; v < nv && 48 + 32 * (v + 1) <= bd.size(); v++)
            {
                const uint8_t *p = bd.data() + 48 + 32 * v;
                double q[3] = {S32(p) / 65536.0, S32(p + 4) / 65536.0, S32(p + 8) / 65536.0}, o[3];
                for (int a = 0; a < 3; a++)
                    o[a] = w[i].r[a][0] * q[0] + w[i].r[a][1] * q[1] + w[i].r[a][2] * q[2] + w[i].t[a];
                for (int a = 0; a < 3; a++)
                {
                    if (first || o[a] < out->restMin[a])
                        out->restMin[a] = o[a];
                    if (first || o[a] > out->restMax[a])
                        out->restMax[a] = o[a];
                }
                first = false;
            }
        }
    }
    return true;
}

std::vector<Mat34> RetargetCel(const StockActor &s, const std::vector<Aff> &rest, const StockCel &cel,
                               const std::vector<std::array<double, 3>> &userPivot, const std::vector<bool> &hasPivot,
                               double motionScale)
{
    const size_t n = s.Parts();
    std::vector<Aff> w = s.World(cel);
    std::vector<Aff> d(n);
    for (size_t i = 0; i < n; i++)
        d[i] = Mul(w[i], Inv(rest[i])); // world-space motion of the part since the rest pose

    // Frank's head, torso, hips and shoes are separate root parts: nothing in the tree ties them together, they only stay
    // together because every matrix was made for his proportions.  The retargeted skeleton gets a virtual tree
    // (hips -> torso -> head, boot -> shoe) so that the pieces keep their attachment when the user's proportions differ.
    std::vector<int> vp(n);
    for (size_t i = 0; i < n; i++)
        vp[i] = s.parent[i];
    if (n == 27)
    {
        vp[0] = 8;
        vp[8] = 18;
        vp[25] = 20;
        vp[26] = 23;
    }
    // which part's pivot a part turns around (the torso turns around the waist = the hips' pivot)
    auto pivotPart = [&](size_t i) -> size_t { return (n == 27 && i == 8 && !hasPivot[8]) ? 18 : i; };

    std::vector<Aff> du(n);
    std::vector<int> state(n, 0);
    std::vector<Aff> eu(n);
    std::vector<size_t> order;
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i])
            return;
        state[i] = 1;
        if (vp[i] >= 0)
            visit((size_t)vp[i]);
        // motion relative to the (virtual) parent, in rest-pose axes
        Aff e = vp[i] < 0 ? d[i] : Mul(Inv(d[(size_t)vp[i]]), d[i]);
        // a rotation turns around a pivot: move the pivot from the stock joint to the user's joint; a scale (Squash,
        // Stretch ...) is about the ground origin and is left alone
        double det = e.r[0][0] * (e.r[1][1] * e.r[2][2] - e.r[1][2] * e.r[2][1]) -
                     e.r[0][1] * (e.r[1][0] * e.r[2][2] - e.r[1][2] * e.r[2][0]) +
                     e.r[0][2] * (e.r[1][0] * e.r[2][1] - e.r[1][1] * e.r[2][0]);
        bool ortho = std::fabs(det - 1) < 0.02;
        for (int a = 0; a < 3 && ortho; a++)
            for (int b = 0; b < 3; b++)
            {
                double dot = 0;
                for (int k = 0; k < 3; k++)
                    dot += e.r[k][a] * e.r[k][b];
                if (std::fabs(dot - (a == b ? 1.0 : 0.0)) > 0.02)
                    ortho = false;
            }
        if (ortho)
        {
            size_t pp = pivotPart(i);
            double pf[3], pu[3];
            for (int a = 0; a < 3; a++)
            {
                pf[a] = (n == 27 && i == 8) ? rest[18].t[a] : rest[pp].t[a]; // the stock waist for the torso
                pu[a] = hasPivot[pp] ? userPivot[pp][a] : pf[a];
            }
            for (int a = 0; a < 3; a++)
            {
                double fo = pf[a], uo = pu[a];
                for (int b = 0; b < 3; b++)
                {
                    fo -= e.r[a][b] * pf[b];
                    uo -= e.r[a][b] * pu[b];
                }
                // translation = rotation about the pivot + a slide (jumps, steps ...) that grows with the actor's size
                e.t[a] = uo + motionScale * (e.t[a] - fo);
            }
        }
        eu[i] = e;
        du[i] = vp[i] < 0 ? e : Mul(du[(size_t)vp[i]], e);
        state[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < n; i++)
        visit(i);

    std::vector<Mat34> out(n);
    for (size_t i = 0; i < n; i++)
    {
        int p = s.parent[i];
        out[i] = ToMat34(p < 0 ? du[i] : Mul(Inv(du[(size_t)p]), du[i]));
    }
    return out;
}

static bool IsFile(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && ((st.st_mode & S_IFMT) == S_IFREG);
}
static bool IsDir(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && ((st.st_mode & S_IFMT) == S_IFDIR);
}

std::string FindStockFile(const std::string &hint, const std::string &modelDir)
{
    std::vector<std::string> cands;
    auto addDir = [&](const std::string &d) {
        if (d.empty())
            return;
        std::string b = d;
        if (b.back() != '/' && b.back() != '\\')
            b += '/';
        cands.push_back(b + "tmpls.3cn");
        cands.push_back(b + "content-files/tmpls.3cn");
        cands.push_back(b + "Content/tmpls.3cn");
        cands.push_back(b + "Microsoft Kids/3D Movie Maker/tmpls.3cn");
    };
    if (!hint.empty())
    {
        if (IsFile(hint))
            return hint;
        addDir(hint);
    }
    addDir(modelDir);
    const char *home = getenv("HOME");
    if (home)
    {
        addDir(std::string(home) + "/3dmmex");
        addDir(std::string(home) + "/.local/share/3dmmex");
        addDir(std::string(home) + "/Microsoft Kids/3D Movie Maker");
    }
#ifdef _WIN32
    for (const char *ev : {"USERPROFILE", "APPDATA", "LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"})
        if (const char *e = getenv(ev))
        {
            addDir(std::string(e) + "/3dmmex");
            addDir(std::string(e) + "/3DMMEx");
            addDir(std::string(e) + "/Microsoft Kids/3D Movie Maker");
        }
    addDir("C:/Program Files (x86)/Microsoft Kids/3D Movie Maker");
    addDir("C:/Program Files/Microsoft Kids/3D Movie Maker");
#endif
    addDir("/usr/share/3dmmex");
    addDir("/usr/local/share/3dmmex");
    addDir("/opt/3dmmex");
    for (const std::string &c : cands)
        if (IsFile(c))
            return c;
    (void)IsDir;
    return std::string();
}

} // namespace dmm
