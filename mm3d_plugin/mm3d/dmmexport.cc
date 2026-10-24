#include "dmmexport.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "../core/geom.h"
#include "../core/imaging.h"
#include "../core/stock.h"
#include "../core/zipwriter.h"

#include "texmgr.h"
#include "texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <strings.h>

using namespace dmm;

// ---------------------------------------------------------------------------------------------- options

static std::string Meta(Model *model, const char *key)
{
    char buf[1024];
    if (model->getMetaData(key, buf, sizeof(buf)))
        return std::string(buf);
    return std::string();
}

static std::string StemOf(const char *path)
{
    std::string p = path ? path : "";
    size_t s = p.find_last_of("/\\");
    if (s != std::string::npos)
        p = p.substr(s + 1);
    size_t d = p.find_last_of('.');
    if (d != std::string::npos && d > 0)
        p = p.substr(0, d);
    return p;
}

static std::string SafeBase(const std::string &in)
{
    std::string out;
    for (char c : in)
    {
        if (isalnum((unsigned char)c) || c == '_' || c == '-')
            out += (char)tolower((unsigned char)c);
        else if (c == ' ')
            out += '_';
    }
    if (out.empty())
        out = "actor";
    if (out.size() > 24)
        out.resize(24);
    return out;
}

static bool Truthy(const std::string &s)
{
    return s == "1" || s == "yes" || s == "true" || s == "on" || s == "Yes" || s == "True";
}

DmmOptions DmmOptionsFromModel(Model *model, const char *outFilename)
{
    DmmOptions o;
    std::string stem = StemOf(outFilename);
    std::string v;
    v = Meta(model, "3DMM_NAME");
    o.name = v.empty() ? stem : v;
    o.author = Meta(model, "3DMM_AUTHOR");
    v = Meta(model, "3DMM_BASE");
    o.base = SafeBase(v.empty() ? stem : v);
    v = Meta(model, "3DMM_TYPE");
    o.prop = (v == "prop" || v == "Prop" || v == "PROP");
    v = Meta(model, "3DMM_FORWARD");
    if (v == "+X" || v == "-X" || v == "+Z" || v == "-Z")
        o.forward = v;
    v = Meta(model, "3DMM_SCALE");
    if (!v.empty())
        o.scale = atof(v.c_str());
    v = Meta(model, "3DMM_HEIGHT");
    if (!v.empty())
    {
        o.fitHeight = atof(v.c_str());
        o.fitHeightSet = true;
    }
    v = Meta(model, "3DMM_MOTION_SCALE");
    if (!v.empty())
        o.motionScale = atof(v.c_str());
    v = Meta(model, "3DMM_ACTOR_ANIMS");
    o.actorAnims = Truthy(v);
    o.stock = Meta(model, "3DMM_STOCK");
    v = Meta(model, "3DMM_STOCK_ACTOR");
    if (!v.empty())
        o.stockActor = v;
    {
        const char *fn = model->getFilename();
        std::string mp = fn ? fn : "";
        size_t sl = mp.find_last_of("/\\");
        o.modelDir = sl == std::string::npos ? std::string() : mp.substr(0, sl);
    }
    v = Meta(model, "3DMM_ORIGIN");
    if (v == "keep")
        o.feetOrigin = false;
    v = Meta(model, "3DMM_DITHER");
    o.dither = Truthy(v);
    v = Meta(model, "3DMM_TEXSIZE");
    if (!v.empty())
        o.texMax = std::max(0, std::min(16384, atoi(v.c_str())));
    v = Meta(model, "3DMM_TEXPOW2");
    o.texPow2 = Truthy(v);
    v = Meta(model, "3DMM_CNO");
    if (!v.empty())
        o.cno = (uint32_t)strtoul(v.c_str(), NULL, 0);
    o.preview = Meta(model, "3DMM_PREVIEW");
    v = Meta(model, "3DMM_DWR");
    if (!v.empty())
        o.dwr = atof(v.c_str());
    v = Meta(model, "3DMM_PROP_ANIMS");
    o.propAnims = Truthy(v);
    v = Meta(model, "3DMM_STEP");
    if (!v.empty())
        o.step = atof(v.c_str());
    v = Meta(model, "3DMM_FIT");
    o.fit = !(v == "0" || v == "off" || v == "no");
    v = Meta(model, "3DMM_FIT_TOL");
    if (!v.empty() && atof(v.c_str()) >= 0)
        o.fitTol = atof(v.c_str());
    v = Meta(model, "3DMM_SKIN");
    o.skinBake = (v == "bake" || v == "Bake");
    v = Meta(model, "3DMM_SKELETON");
    o.skeleton = !(v == "0" || v == "off" || v == "no" || v == "none");
    v = Meta(model, "3DMM_ANIMS");
    o.skipAnims = (v == "none" || v == "off" || v == "0");
    return o;
}

// ---------------------------------------------------------------------------------------------- helpers

namespace
{

struct Xform
{
    int mode = 0; // forward axis: 0 = -X, 1 = +X, 2 = +Z, 3 = -Z (what ends up on 3DMM's -X)
    double s = 1;
    double t[3] = {0, 0, 0};
    void Rot(const double p[3], double out[3]) const
    {
        switch (mode)
        {
        case 0: // forward -X: identity
            out[0] = p[0];
            out[2] = p[2];
            break;
        case 1: // forward +X: 180 about Y
            out[0] = -p[0];
            out[2] = -p[2];
            break;
        case 2: // forward +Z -> -X
            out[0] = -p[2];
            out[2] = p[0];
            break;
        default: // forward -Z -> -X
            out[0] = p[2];
            out[2] = -p[0];
            break;
        }
        out[1] = p[1];
    }
    void Apply(const double p[3], float out[3]) const
    {
        double r[3];
        Rot(p, r);
        for (int i = 0; i < 3; i++)
            out[i] = (float)(r[i] * s + t[i]);
    }
};

// Least-effort affine fit: finds A (row vectors, p' = p*A + t) mapping the rest points onto the posed points, using four
// well-spread points (a synthetic fourth one for planar sets) and then checking every point against the tolerance.
bool AffineFit(const std::vector<std::array<double, 3>> &P, const std::vector<std::array<double, 3>> &Q, double tol, Aff *out)
{
    const size_t n = P.size();
    if (n == 0 || Q.size() != n)
        return false;
    auto sub = [](const std::array<double, 3> &a, const std::array<double, 3> &b) {
        return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    auto dot = [](const std::array<double, 3> &a, const std::array<double, 3> &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const std::array<double, 3> &a, const std::array<double, 3> &b) {
        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto len = [&](const std::array<double, 3> &a) { return std::sqrt(dot(a, a)); };
    const double eps = 1e-4;
    // pure translation?
    auto translation = [&]() {
        std::array<double, 3> d = sub(Q[0], P[0]);
        Aff a;
        for (int i = 0; i < 3; i++)
            a.t[i] = d[i];
        for (size_t k = 0; k < n; k++)
            if (len(sub(sub(Q[k], P[k]), d)) > tol)
                return false;
        *out = a;
        return true;
    };
    if (translation())
        return true;
    // pick a (farthest from p0), b (farthest from line), c (farthest from plane)
    size_t ia = 0, ib = 0, ic = 0;
    double best = 0;
    for (size_t k = 1; k < n; k++)
        if (len(sub(P[k], P[0])) > best)
            best = len(sub(P[k], P[0])), ia = k;
    if (best < eps)
        return false;
    std::array<double, 3> u = sub(P[ia], P[0]);
    best = 0;
    for (size_t k = 1; k < n; k++)
    {
        double l = len(cross(u, sub(P[k], P[0]))) / len(u);
        if (l > best)
            best = l, ib = k;
    }
    if (best < eps)
        return false; // collinear: leave it to a new model
    std::array<double, 3> v = sub(P[ib], P[0]);
    std::array<double, 3> nrm = cross(u, v);
    double nl = len(nrm);
    best = 0;
    for (size_t k = 1; k < n; k++)
    {
        double l = std::fabs(dot(nrm, sub(P[k], P[0]))) / nl;
        if (l > best)
            best = l, ic = k;
    }
    std::array<double, 3> qu = sub(Q[ia], Q[0]), qv = sub(Q[ib], Q[0]), w, qw;
    if (best < eps * 10)
    {   // planar: the posed normal is the cross product of the posed in-plane vectors
        std::array<double, 3> qn = cross(qu, qv);
        double ql = len(qn);
        if (ql < 1e-9)
            return false;
        double s = std::sqrt(ql / nl);
        for (int i = 0; i < 3; i++)
        {
            w[i] = nrm[i] / nl;
            qw[i] = qn[i] / ql * s;
        }
    }
    else
    {
        w = sub(P[ic], P[0]);
        qw = sub(Q[ic], Q[0]);
    }
    // solve [u;v;w] * A = [qu;qv;qw]
    double M[3][3] = {{u[0], u[1], u[2]}, {v[0], v[1], v[2]}, {w[0], w[1], w[2]}};
    double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                 M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (std::fabs(det) < 1e-12)
        return false;
    double inv[3][3];
    inv[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det;
    inv[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det;
    inv[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
    inv[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det;
    inv[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det;
    inv[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det;
    inv[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det;
    inv[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det;
    inv[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
    double R[3][3]; // row-vector matrix A
    const double *qs[3] = {qu.data(), qv.data(), qw.data()};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            R[i][j] = inv[i][0] * qs[0][j] + inv[i][1] * qs[1][j] + inv[i][2] * qs[2][j];
    double dA = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    if (std::fabs(dA) < 1e-3 || std::fabs(dA) > 1e3)
        return false;
    double t[3];
    for (int j = 0; j < 3; j++)
        t[j] = Q[0][j] - (P[0][0] * R[0][j] + P[0][1] * R[1][j] + P[0][2] * R[2][j]);
    for (size_t k = 0; k < n; k++)
        for (int j = 0; j < 3; j++)
        {
            double e = P[k][0] * R[0][j] + P[k][1] * R[1][j] + P[k][2] * R[2][j] + t[j] - Q[k][j];
            if (std::fabs(e) > tol)
                return false;
        }
    Aff a; // column convention: out = r*p + t, r = A^T
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            a.r[i][j] = R[j][i];
        a.t[i] = t[i];
    }
    *out = a;
    return true;
}

struct PartTopo
{
    int joint = -1; // skeletal export: the joint that owns this piece (-1 = none)
    std::string name;
    int set = 0;
    int material = -1;
    std::vector<unsigned> srcVertex; // local vertex -> model vertex
    std::vector<float> u, v;
    std::vector<std::array<uint16_t, 3>> faces;
};

RgbaImage ToRgba(const Texture *tex)
{
    RgbaImage img;
    img.w = tex->m_width;
    img.h = tex->m_height;
    img.px.assign((size_t)img.w * img.h * 4, 255);
    int bpp = (tex->m_format == Texture::FORMAT_RGBA) ? 4 : 3;
    for (int y = 0; y < img.h; y++)
    {
        // MM3D keeps row 0 at t = 0 (the bottom of the picture); our images are top-down
        const uint8_t *src = tex->m_data + (size_t)(img.h - 1 - y) * img.w * bpp;
        for (int x = 0; x < img.w; x++)
        {
            uint8_t *o = &img.px[((size_t)y * img.w + x) * 4];
            o[0] = src[x * bpp + 0];
            o[1] = src[x * bpp + 1];
            o[2] = src[x * bpp + 2];
            o[3] = (bpp == 4) ? src[x * bpp + 3] : 255;
        }
    }
    return img;
}

ImagePtr SolidImage(int r, int g, int b)
{
    auto img = std::make_shared<Image8>();
    img->w = img->h = 8;
    img->px.assign(64, (uint8_t)NearestIndex(r, g, b));
    return img;
}

// material names may carry a path and/or extension (MM3D names a material after its texture file)
std::string NormName(const std::string &in)
{
    std::string n = in;
    size_t s = n.find_last_of("/\\");
    if (s != std::string::npos)
        n = n.substr(s + 1);
    size_t d = n.find_last_of('.');
    if (d != std::string::npos && d > 0 && n.size() - d <= 5)
        n = n.substr(0, d);
    return n;
}

// 3DMM_DITHER:<material name> = 0|1 overrides the global dither setting for one texture
bool DitherFor(Model *model, const std::string &matName, bool global)
{
    std::string n = NormName(matName);
    std::string base = n.substr(0, n.find('~')); // skins inherit from their base material
    const std::string cands[4] = {matName, n, std::string(), base};
    for (int i = 0; i < 4; i++)
    {
        if (i == 2)
            continue;
        std::string v = Meta(model, ("3DMM_DITHER:" + cands[i]).c_str());
        if (!v.empty())
            return Truthy(v);
    }
    return global;
}

std::string BaseOfVariant(const std::string &name)
{
    std::string n = NormName(name);
    size_t p = n.find('~');
    return (p == std::string::npos) ? std::string() : n.substr(0, p);
}

} // namespace


// ---------------------------------------------------------------------------------------------- default actor mode

namespace
{

// Names understood in default actor mode.  Frank's 27 parts are the target; left = +Z in 3DMM space (the character faces -X).
struct NameEntry
{
    const char *base;
    int l, r; // left and right part; r < 0 means a single part
};
const NameEntry kNames[] = {
    {"head", 0, -1},      {"skull", 0, -1},      {"face", 0, -1},       {"neck", 0, -1},       {"hat", 7, -1},
    {"cap", 7, -1},       {"helmet", 7, -1},     {"accessory", 7, -1},  {"glasses", 7, -1},    {"sunglasses", 7, -1},
    {"crown", 7, -1},     {"headwear", 7, -1},   {"torso", 8, -1},      {"body", 8, -1},       {"chest", 8, -1},
    {"trunk", 8, -1},     {"spine", 8, -1},      {"belly", 8, -1},      {"stomach", 8, -1},    {"abdomen", 8, -1},
    {"torso back", 9, -1}, {"body back", 9, -1}, {"collar", 10, -1},    {"shoulders", 10, -1}, {"collar back", 11, -1},
    {"ear", 1, 3},        {"ear tip", 2, 4},     {"eye", 5, 6},         {"eyes", 5, 6},        {"eyeball", 5, 6},
    {"upper arm", 17, 14}, {"shoulder", 17, 14}, {"arm", 17, 14},       {"bicep", 17, 14},     {"forearm", 15, 12},
    {"lower arm", 15, 12}, {"elbow", 15, 12},    {"hand", 16, 13},      {"wrist", 16, 13},     {"palm", 16, 13},
    {"fist", 16, 13},     {"finger", 16, 13},    {"fingers", 16, 13},   {"hips", 18, -1},      {"hip", 18, -1},
    {"pelvis", 18, -1},   {"waist", 18, -1},     {"thigh", 21, 24},     {"upper leg", 21, 24}, {"leg", 21, 24},
    {"knee", 19, 22},     {"shin", 19, 22},      {"calf", 19, 22},      {"lower leg", 19, 22}, {"ankle", 20, 23},
    {"boot", 20, 23},     {"foot", 25, 26},      {"feet", 25, 26},      {"shoe", 25, 26},      {"sole", 25, 26},
    {"toe", 25, 26},      {"toes", 25, 26}};

// lower-case words of a name (camelCase split, digits dropped); *side = +1 left, -1 right, 0 none
std::vector<std::string> NameWords(const std::string &in, int *side)
{
    std::string s;
    for (size_t i = 0; i < in.size(); i++)
    {
        if (i > 0 && isupper((unsigned char)in[i]) && islower((unsigned char)in[i - 1]))
            s += ' ';
        s += in[i];
    }
    std::vector<std::string> out;
    std::string cur;
    *side = 0;
    auto flush = [&]() {
        if (cur.empty())
            return;
        if (cur == "l" || cur == "left" || cur == "lt")
            *side = 1;
        else if (cur == "r" || cur == "right" || cur == "rt")
            *side = -1;
        else if (cur.find_first_not_of("0123456789") != std::string::npos)
            out.push_back(cur);
        cur.clear();
    };
    for (char ch : s)
    {
        if (isalnum((unsigned char)ch))
            cur += (char)tolower((unsigned char)ch);
        else
            flush();
    }
    flush();
    return out;
}

// stock part for a joint or group name, -1 when unknown; sideHint (+1/-1) decides when the name has no side word
int ResolveName(const std::string &name, int sideHint)
{
    int side;
    std::vector<std::string> w = NameWords(name, &side);
    for (size_t start = 0; start < w.size(); start++)
    {
        std::string base;
        for (size_t k = start; k < w.size(); k++)
            base += (k > start ? " " : "") + w[k];
        for (const NameEntry &e : kNames)
            if (base == e.base)
            {
                if (e.r < 0)
                    return e.l;
                return (side != 0 ? side : sideHint) > 0 ? e.l : e.r;
            }
    }
    return -1;
}

bool IsHinge(int part) // parts whose model origin is a real joint (the torso and the shoes are placed at the ground)
{
    return !(part == 7 || part == 8 || part == 9 || part == 25 || part == 26);
}

struct PartGeom
{
    std::map<std::array<unsigned, 3>, uint16_t> local;
    std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>();
    std::map<int, int> matFaces; // material -> number of faces
    int Add(unsigned v, float s, float t, const std::array<float, 3> &pos)
    {
        unsigned sb, tb;
        memcpy(&sb, &s, 4);
        memcpy(&tb, &t, 4);
        std::array<unsigned, 3> key = {v, sb, tb};
        auto it = local.find(key);
        if (it != local.end())
            return it->second;
        int idx = (int)mesh->verts.size();
        if (idx >= 65535)
            return -1;
        mesh->verts.push_back(Vertex{pos[0], pos[1], pos[2], s, t});
        local[key] = (uint16_t)idx;
        return idx;
    }
    int BestMaterial() const
    {
        int best = -1, n = -1;
        for (const auto &kv : matFaces)
            if (kv.second > n)
                best = kv.first, n = kv.second;
        return best;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------- the mapping

bool DmmBuildSpec(Model *model, const DmmOptions &opt, Spec *spec, std::string *err, std::string *log)
{
    auto fail = [&](const std::string &m) {
        if (err)
            *err = m;
        return false;
    };
    auto note = [&](const std::string &m) {
        if (log)
            *log += m + "\n";
    };

    const unsigned nverts = (unsigned)model->getVertexCount();
    const unsigned ntris = (unsigned)model->getTriangleCount();
    if (nverts == 0 || ntris == 0)
        return fail("the model is empty");
    // the picker thumbnail from the 3DMM_PREVIEW image, when there is one
    auto tryPreview = [&](Image8 *thumb) -> bool {
        bool havePreview = false;
        if (opt.preview.empty())
            return false;
        {
            std::string path = opt.preview;
            FILE *f = fopen(path.c_str(), "rb");
            if (!f && path[0] != '/' && path[0] != '\\' && !(path.size() > 1 && path[1] == ':'))
            {
                std::string mp = model->getFilename() ? model->getFilename() : "";
                size_t sl = mp.find_last_of("/\\");
                if (sl != std::string::npos)
                    path = mp.substr(0, sl + 1) + opt.preview;
                f = fopen(path.c_str(), "rb");
            }
            if (f)
            {
                fclose(f);
                Texture *tex = TextureManager::getInstance()->getTexture(path.c_str(), true, false);
                if (tex && !tex->m_isBad && tex->m_data)
                {
                    *thumb = MakeThumbFromImage(ToRgba(tex));
                    havePreview = true;
                }
            }
            if (!havePreview)
                note("preview image '" + opt.preview + "' could not be loaded; rendering one instead");
        }
        return havePreview;
    };


    // default actor mode: the stock actor supplies the body parts and the animations
    StockActor stockActor;
    double stockHeight = 0;
    if (opt.actorAnims)
    {
        std::string path = FindStockFile(opt.stock, opt.modelDir), serr;
        if (path.empty())
            return fail("default actor mode needs the game's tmpls.3cn: set the 3DMM_STOCK model metadata to its path "
                        "(or the folder that holds it)");
        if (!LoadStockActor(path, opt.stockActor, &stockActor, &serr))
            return fail("could not read the stock actor: " + serr);
        if (stockActor.Parts() != 27)
            return fail("default actor mode is built for Frank's 27-part layout; '" + opt.stockActor + "' has " +
                        std::to_string(stockActor.Parts()) + " parts");
        stockHeight = stockActor.restMax[1] - stockActor.restMin[1];
        note("stock actor " + stockActor.name + " from " + path);
    }

    // ---- parts from groups (an optional "#N" suffix on the group name selects the body part set).
    // With a skeleton, every triangle also belongs to the joint that most of its vertices are bound to, and a group
    // is split into one piece per joint.
    const int njoints = opt.skeleton ? model->getBoneJointCount() : 0;
    const bool skeletal = njoints > 0;
    std::vector<PartTopo> topo;
    {
        int groups = model->getGroupCount();
        std::vector<int> triGroup(ntris, -1), triJoint(ntris, -1);
        for (unsigned t = 0; t < ntris; t++)
        {
            triGroup[t] = model->getTriangleGroup(t);
            if (!skeletal)
                continue;
            int j[3];
            for (unsigned c = 0; c < 3; c++)
                j[c] = model->getPrimaryVertexInfluence((unsigned)model->getTriangleVertex(t, c));
            if (j[0] == j[1] || j[0] == j[2])
                triJoint[t] = j[0];
            else if (j[1] == j[2])
                triJoint[t] = j[1];
            else
                triJoint[t] = j[0] >= 0 ? j[0] : (j[1] >= 0 ? j[1] : j[2]);
        }
        std::map<std::pair<int, int>, std::vector<unsigned>> pieces; // (joint, group) -> triangles
        for (unsigned t = 0; t < ntris; t++)
            pieces[std::make_pair(triJoint[t], triGroup[t])].push_back(t);
        for (const auto &piece : pieces)
        {
            const int jt = piece.first.first, g = piece.first.second;
            const std::vector<unsigned> &tris = piece.second;
            PartTopo p;
            p.joint = jt;
            if (g < 0)
            {
                p.name = "ungrouped";
                p.material = -1;
            }
            else
            {
                std::string nm = model->getGroupName((unsigned)g);
                size_t h = nm.rfind('#');
                if (h != std::string::npos && h + 1 < nm.size() &&
                    nm.find_first_not_of("0123456789", h + 1) == std::string::npos)
                {
                    p.set = atoi(nm.c_str() + h + 1);
                    nm = nm.substr(0, h);
                    while (!nm.empty() && nm.back() == ' ')
                        nm.pop_back();
                }
                p.name = nm.empty() ? "part" : nm;
                p.material = model->getGroupTextureId((unsigned)g);
            }
            std::map<std::array<unsigned, 3>, uint16_t> local; // (vertex, s bits, t bits)
            for (unsigned t : tris)
            {
                std::array<uint16_t, 3> face;
                for (unsigned c = 0; c < 3; c++)
                {
                    unsigned v = (unsigned)model->getTriangleVertex(t, c);
                    float s = 0, tt = 0;
                    model->getTextureCoords(t, c, s, tt);
                    unsigned sb, tb;
                    memcpy(&sb, &s, 4);
                    memcpy(&tb, &tt, 4);
                    std::array<unsigned, 3> key = {v, sb, tb};
                    auto it = local.find(key);
                    if (it == local.end())
                    {
                        if (p.srcVertex.size() >= 65535)
                            return fail("group '" + p.name + "' needs more than 65535 vertices");
                        uint16_t idx = (uint16_t)p.srcVertex.size();
                        p.srcVertex.push_back(v);
                        p.u.push_back(s);
                        p.v.push_back(tt);
                        it = local.insert(std::make_pair(key, idx)).first;
                    }
                    face[c] = it->second;
                }
                p.faces.push_back(face);
            }
            topo.push_back(p);
        }
    }
    if (topo.empty())
        return fail("no triangles to export");

    // dense body part set numbers; the root part lives in set 0 together with every part that has no suffix
    std::vector<int> setIds;
    setIds.push_back(0);
    for (const PartTopo &p : topo)
        setIds.push_back(p.set);
    std::sort(setIds.begin(), setIds.end());
    setIds.erase(std::unique(setIds.begin(), setIds.end()), setIds.end());
    auto denseSet = [&](int s) { return (int)(std::lower_bound(setIds.begin(), setIds.end(), s) - setIds.begin()); };
    const int nsets = (int)setIds.size();

    // ---- transform: rotate the chosen front axis onto 3DMM's forward (-X), fit, and put the feet on the ground
    Xform xf;
    double userHeight = 0; // height of the exported model in 3DMM units
    xf.mode = opt.forward == "-X" ? 0 : opt.forward == "+X" ? 1 : opt.forward == "+Z" ? 2 : 3;
    {
        double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
        for (unsigned v = 0; v < nverts; v++)
        {
            double c[3], r[3];
            model->getVertexCoordsUnanimated(v, c);
            xf.Rot(c, r);
            for (int i = 0; i < 3; i++)
            {
                mn[i] = std::min(mn[i], r[i]);
                mx[i] = std::max(mx[i], r[i]);
            }
        }
        double height = mx[1] - mn[1];
        if (opt.scale > 0)
            xf.s = opt.scale;
        else if (opt.actorAnims && !opt.fitHeightSet && stockHeight > 1e-6 && height > 1e-9)
            xf.s = stockHeight / height; // the stock actor's own height
        else if (opt.fitHeight > 0 && height > 1e-9)
            xf.s = opt.fitHeight / height;
        userHeight = height * xf.s;
        if (opt.feetOrigin)
        {
            xf.t[0] = -(mn[0] + mx[0]) / 2 * xf.s;
            xf.t[1] = -mn[1] * xf.s;
            xf.t[2] = -(mn[2] + mx[2]) / 2 * xf.s;
        }
        char b[160];
        snprintf(b, sizeof(b), "size %.2f x %.2f x %.2f model units, scale %.4f -> %.2f units tall", mx[0] - mn[0],
                 mx[1] - mn[1], mx[2] - mn[2], xf.s, height * xf.s);
        note(b);
    }

    // ---- textures and skins
    const int nmat = model->getTextureCount();
    std::map<int, ImagePtr> imageOf; // material -> palette image
    auto imageFor = [&](int mat) -> ImagePtr {
        auto it = imageOf.find(mat);
        if (it != imageOf.end())
            return it->second;
        ImagePtr res;
        if (mat < 0 || mat >= nmat)
            res = SolidImage(170, 170, 170);
        else if (model->getMaterialType((unsigned)mat) == Model::Material::MATTYPE_TEXTURE)
        {
            Texture *tex = model->getTextureData((unsigned)mat);
            if (tex && !tex->m_isBad && tex->m_data && tex->m_width > 0 && tex->m_height > 0)
            {
                RgbaImage rgba = ToRgba(tex);
                int w = rgba.w, h = rgba.h;
                if (opt.texMax > 0 && std::max(w, h) > opt.texMax)
                {
                    double s = (double)opt.texMax / std::max(w, h);
                    w = std::max(1, (int)std::lround(w * s));
                    h = std::max(1, (int)std::lround(h * s));
                }
                if (opt.texPow2)
                {
                    w = PowerOfTwoSize(w, 4, 1 << 14);
                    h = PowerOfTwoSize(h, 4, 1 << 14);
                    while (opt.texMax > 0 && w > opt.texMax && w > 4)
                        w >>= 1;
                    while (opt.texMax > 0 && h > opt.texMax && h > 1)
                        h >>= 1;
                }
                w = (w + 3) & ~3; // stock textures all have rows that are a multiple of 4 pixels
                if (w != rgba.w || h != rgba.h)
                    rgba = Resample(rgba, w, h);
                res = std::make_shared<Image8>(Quantize(rgba, DitherFor(model, model->getTextureName((unsigned)mat), opt.dither)));
            }
            else
            {
                note(std::string("material '") + model->getTextureName((unsigned)mat) + "' has no usable texture");
                res = SolidImage(170, 170, 170);
            }
        }
        else
        {
            float d[4] = {0.7f, 0.7f, 0.7f, 1};
            model->getTextureDiffuse((unsigned)mat, d);
            res = SolidImage((int)std::lround(d[0] * 255), (int)std::lround(d[1] * 255), (int)std::lround(d[2] * 255));
        }
        imageOf[mat] = res;
        return res;
    };
    // variants: a material called "base~anything" is an extra skin for the material called "base"
    std::map<int, std::vector<int>> variantsOf;
    for (int m = 0; m < nmat; m++)
    {
        std::string b = BaseOfVariant(model->getTextureName((unsigned)m));
        if (b.empty())
            continue;
        for (int o = 0; o < nmat; o++)
        {
            std::string on = NormName(model->getTextureName((unsigned)o));
            if (o != m && on.find('~') == std::string::npos && strcasecmp(on.c_str(), b.c_str()) == 0)
            {
                variantsOf[o].push_back(m);
                break;
            }
        }
    }

    // ======== default actor mode: the user's geometry rides on the stock actor's body parts and animations ========
    if (opt.actorAnims)
    {
        Spec &S = *spec;
        S = Spec();
        const StockActor &sa = stockActor;
        const int np = (int)sa.Parts();
        S.name = opt.name;
        S.prop = false;
        for (int i = 0; i < 3; i++)
            S.rest[i] = sa.rest[i];

        std::vector<std::array<float, 3>> rest(nverts);
        for (unsigned v = 0; v < nverts; v++)
        {
            double c[3];
            model->getVertexCoordsUnanimated(v, c);
            xf.Apply(c, rest[v].data());
        }
        // the geometry is placed in the pose of the user's "At Rest" animation (first frame) when there is one
        std::vector<std::array<double, 3>> posedJoint(njoints);
        bool haveRestPose = false, posedJointsValid = false;
        if (!opt.skipAnims)
        {
            for (unsigned an = 0; an < model->getAnimCount(Model::ANIMMODE_SKELETAL) && !haveRestPose; an++)
                if (strcasecmp(model->getAnimName(Model::ANIMMODE_SKELETAL, an), "At Rest") == 0 &&
                    model->getAnimFrameCount(Model::ANIMMODE_SKELETAL, an) > 0 && model->setCurrentAnimation(Model::ANIMMODE_SKELETAL, an))
                {
                    model->setCurrentAnimationFrame(0);
                    for (unsigned v = 0; v < nverts; v++)
                    {
                        double c[3];
                        model->getVertexCoords(v, c);
                        xf.Apply(c, rest[v].data());
                    }
                    for (int j = 0; j < njoints; j++)
                    {
                        Matrix fm;
                        double o[3] = {0, 0, 0};
                        if (model->getBoneJointFinalMatrix((unsigned)j, fm))
                            fm.apply3x(o);
                        float p[3];
                        xf.Apply(o, p);
                        posedJoint[j] = {p[0], p[1], p[2]};
                    }
                    model->setNoAnimation();
                    haveRestPose = true;
                    posedJointsValid = true;
                    note("geometry placed in the pose of your 'At Rest' skeletal animation");
                }
            for (unsigned an = 0; an < model->getAnimCount(Model::ANIMMODE_FRAME) && !haveRestPose; an++)
                if (strcasecmp(model->getAnimName(Model::ANIMMODE_FRAME, an), "At Rest") == 0 &&
                    model->getAnimFrameCount(Model::ANIMMODE_FRAME, an) > 0)
                {
                    for (unsigned v = 0; v < nverts; v++)
                    {
                        double c[3];
                        if (!model->getFrameAnimVertexCoords(an, 0, v, c[0], c[1], c[2]))
                            model->getVertexCoordsUnanimated(v, c);
                        xf.Apply(c, rest[v].data());
                    }
                    haveRestPose = true;
                    note("geometry placed in the pose of your 'At Rest' frame animation");
                }
        }
        const std::vector<Aff> restW = sa.World(sa.actions[0].cels[0]);
        int hatPart = -1;
        for (int i = 0; i < np; i++)
            if (sa.actions[0].cels[0].model[(size_t)i] < 0)
                hatPart = i;

        auto resolve = [&](const std::string &nm, int hint) -> int {
            std::string mv = Meta(model, ("3DMM_MAP:" + nm).c_str());
            if (!mv.empty())
            {
                if (isdigit((unsigned char)mv[0]))
                {
                    int p = atoi(mv.c_str());
                    return (p >= 0 && p < np) ? p : -1;
                }
                return ResolveName(mv, hint);
            }
            return ResolveName(nm, hint);
        };

        // joints: positions in 3DMM space, the stock part each name stands for
        std::vector<std::array<double, 3>> jointPos(njoints);
        std::vector<std::string> jointName(njoints);
        std::vector<int> jointDirect(njoints, -1);
        for (int j = 0; j < njoints; j++)
        {
            double c[3];
            float p[3];
            model->getBoneJointCoords((unsigned)j, c);
            xf.Apply(c, p);
            for (int i = 0; i < 3; i++)
                jointPos[j][i] = posedJointsValid ? posedJoint[j][i] : p[i];
            jointName[j] = model->getBoneJointName((unsigned)j) ? model->getBoneJointName((unsigned)j) : "";
            jointDirect[j] = resolve(jointName[j], jointPos[j][2] >= 0 ? 1 : -1);
        }
        {   // a left/right word that disagrees with where the joint sits means the stock motion would be mirrored
            std::string bad;
            int nbad = 0;
            for (int j = 0; j < njoints; j++)
            {
                int sd;
                NameWords(jointName[j], &sd);
                if (sd != 0 && jointDirect[j] >= 0 && fabs(jointPos[j][2]) > 0.3 && (jointPos[j][2] > 0) != (sd > 0))
                {
                    nbad++;
                    if (bad.size() < 120)
                        bad += (bad.empty() ? "" : ", ") + jointName[j];
                }
            }
            if (nbad)
                note("WARNING: " + std::to_string(nbad) + " joint(s) are on the opposite side from their name (" + bad +
                     "): the stock arm/leg motion will be mirrored. The character's left is +X for a model facing +Z (see 3DMM_FORWARD); rename them or the model's side.");
        }
        std::vector<std::array<double, 3>> userPivot(np);
        std::vector<bool> hasPivot(np, false);
        for (int j = 0; j < njoints; j++)
        {
            int p = jointDirect[j];
            if (p >= 0 && (IsHinge(p) || p == 8) && !hasPivot[(size_t)p])
            {
                hasPivot[(size_t)p] = true;
                userPivot[(size_t)p] = jointPos[j];
            }
        }
        auto partOfJoint = [&](int j) {
            for (int q = j, guard = 0; q >= 0 && guard < njoints + 1; q = model->getBoneJointParent((unsigned)q), guard++)
                if (jointDirect[q] >= 0)
                    return jointDirect[q];
            return -1;
        };

        // triangles -> stock parts
        std::vector<PartGeom> geom((size_t)np);
        std::map<int, PartGeom> hats; // accessory alternatives (group "hat@2" ...)
        std::vector<std::string> unmapped;
        std::vector<std::string> mapLog((size_t)np);
        // the stock hip line, in the user's proportions: the spine (waist) joint, else the hip joint, else scaled to the actor's height
        const double stockHt = sa.restMax[1] - sa.restMin[1];
        const double hipY = hasPivot[8] ? userPivot[8][1] : hasPivot[18] ? userPivot[18][1]
                                         : restW[18].t[1] * ((stockHt > 1e-6 && userHeight > 1e-6) ? userHeight / stockHt : 1.0);
        for (size_t ti = 0; ti < topo.size(); ti++)
        {
            const PartTopo &pc = topo[ti];
            std::string gname = pc.name;
            int alt = 1;
            size_t at = gname.rfind('@');
            if (at != std::string::npos && at + 1 < gname.size() && gname.find_first_not_of("0123456789", at + 1) == std::string::npos)
            {
                alt = atoi(gname.c_str() + at + 1);
                gname = gname.substr(0, at);
            }
            while (!gname.empty() && gname.back() == ' ')
                gname.pop_back();
            double cz = 0;
            for (unsigned sv : pc.srcVertex)
                cz += rest[sv][2];
            int hint = (pc.srcVertex.empty() || cz / pc.srcVertex.size() >= 0) ? 1 : -1;
            int part = -1;
            bool isHat = false;
            int g = resolve(gname, hint);
            if (hatPart >= 0 && g == 7)
            {
                part = hatPart;
                isHat = true;
            }
            if (part < 0 && skeletal && pc.joint >= 0)
                part = partOfJoint(pc.joint);
            if (part < 0 && (!skeletal || pc.joint < 0) && g >= 0 && g != 7)
                part = g;
            if (part < 0)
            {
                part = 8;
                std::string label = gname + (pc.joint >= 0 ? " (joint " + jointName[pc.joint] + ")" : "");
                if (std::find(unmapped.begin(), unmapped.end(), label) == unmapped.end())
                    unmapped.push_back(label);
            }
            for (const auto &f : pc.faces)
            {
                int tp = part;
                if (!isHat && tp == 18)
                {   // the root joint usually carries the whole upper body; above the stock hip line it is torso
                    double y = (rest[pc.srcVertex[f[0]]][1] + rest[pc.srcVertex[f[1]]][1] + rest[pc.srcVertex[f[2]]][1]) / 3;
                    if (y > hipY)
                        tp = 8;
                }
                PartGeom &pg = isHat ? hats[alt] : geom[(size_t)tp];
                int idx[3];
                for (int k = 0; k < 3; k++)
                {
                    uint16_t lv = f[(size_t)k];
                    idx[k] = pg.Add(pc.srcVertex[lv], pc.u[lv], pc.v[lv], rest[pc.srcVertex[lv]]);
                    if (idx[k] < 0)
                        return fail("a body part needs more than 65535 vertices");
                }
                pg.mesh->faces.push_back({(uint16_t)idx[0], (uint16_t)idx[1], (uint16_t)idx[2]});
                pg.matFaces[pc.material]++;
                std::string &ml = mapLog[(size_t)(isHat ? hatPart : tp)];
                std::string src = pc.joint >= 0 ? jointName[pc.joint] : gname;
                if (ml.find(src) == std::string::npos)
                    ml += (ml.empty() ? "" : ", ") + src;
            }
        }

        // models
        S.models.push_back(std::make_shared<Mesh>()); // model 0: blank
        std::vector<int> partModel((size_t)np, 0);
        for (int i = 0; i < np; i++)
            if (!geom[(size_t)i].mesh->faces.empty())
            {
                partModel[(size_t)i] = (int)S.models.size();
                S.models.push_back(geom[(size_t)i].mesh);
            }
        const bool hasHat = hatPart >= 0 && !hats.empty();

        // parts and costumes (body part sets come from the stock actor)
        int nsets = 0;
        for (int i = 0; i < np; i++)
        {
            Part part;
            part.name = "stock part " + std::to_string(i);
            part.parent = sa.parent[(size_t)i];
            part.bset = sa.bset[(size_t)i];
            nsets = std::max(nsets, part.bset + 1);
            S.parts.push_back(part);
        }
        auto skinMat = [&](int mat, size_t k) { // k = 0 the material itself, k > 0 its k-th ~skin
            if (mat >= 0 && k > 0)
            {
                auto it = variantsOf.find(mat);
                if (it != variantsOf.end() && k - 1 < it->second.size())
                    return it->second[k - 1];
            }
            return mat;
        };
        auto skinCount = [&](int mat) -> size_t {
            auto it = variantsOf.find(mat);
            return it == variantsOf.end() ? 1 : it->second.size() + 1;
        };
        S.costumes.assign((size_t)nsets, std::vector<std::map<int, Look>>());
        for (int s = 0; s < nsets; s++)
        {
            const bool hatSet = hasHat && sa.bset[(size_t)hatPart] == s;
            if (hatSet)
            {
                std::vector<int> keys;
                for (auto &kv : hats)
                    keys.push_back(kv.first);
                for (size_t ai = 0; ai < keys.size(); ai++)
                {
                    PartGeom &pg = hats[keys[ai]];
                    S.accModels.push_back(pg.mesh);
                    int mat = pg.BestMaterial();
                    for (size_t k = 0; k < skinCount(mat); k++)
                    {
                        Look look;
                        look.texture = imageFor(skinMat(mat, k));
                        look.accModel = (int)ai;
                        S.costumes[(size_t)s].push_back(std::map<int, Look>{{hatPart, look}});
                    }
                }
                S.accModels.push_back(std::make_shared<Mesh>()); // "no hat", like Patrick's glasses
                Look none;
                none.accModel = (int)keys.size();
                S.costumes[(size_t)s].push_back(std::map<int, Look>{{hatPart, none}});
                char b[96];
                snprintf(b, sizeof(b), "accessory set %d: %zu hat model(s) + none, %zu costumes", s, keys.size(), S.costumes[(size_t)s].size());
                note(b);
                continue;
            }
            size_t ncost = 1;
            for (int i = 0; i < np; i++)
                if (sa.bset[(size_t)i] == s && !geom[(size_t)i].mesh->faces.empty())
                    ncost = std::max(ncost, skinCount(geom[(size_t)i].BestMaterial()));
            for (size_t k = 0; k < ncost; k++)
            {
                std::map<int, Look> costume;
                for (int i = 0; i < np; i++)
                    if (sa.bset[(size_t)i] == s && !geom[(size_t)i].mesh->faces.empty())
                    {
                        Look look;
                        look.texture = imageFor(skinMat(geom[(size_t)i].BestMaterial(), k));
                        costume[i] = look;
                    }
                S.costumes[(size_t)s].push_back(costume);
            }
            if (ncost > 1)
            {
                char b[96];
                snprintf(b, sizeof(b), "body part set %d: %zu skins", s, ncost);
                note(b);
            }
        }

        // actions: the stock cels with the user's models, matrices turned around the user's own joints
        double stockH = sa.restMax[1] - sa.restMin[1];
        double motionScale = opt.motionScale > 0 ? opt.motionScale : (stockH > 1e-6 && userHeight > 1e-6 ? userHeight / stockH : 1.0);
        if (fabs(motionScale - 1) > 0.02)
        {
            char mb[96];
            snprintf(mb, sizeof(mb), "the stock motion is scaled by %.2f to match the actor's height", motionScale);
            note(mb);
        }
        for (const StockAction &sact : sa.actions)
        {
            Action a;
            a.name = sact.name;
            a.flagsRaw = sact.flags;
            for (const StockCel &cel : sact.cels)
            {
                a.celFixed.push_back({cel.fixed[0], cel.fixed[1]});
                std::vector<Mat34> mats = RetargetCel(sa, restW, cel, userPivot, hasPivot, motionScale);
                std::vector<CelPart> cp((size_t)np);
                for (int i = 0; i < np; i++)
                {
                    cp[(size_t)i].model = (cel.model[(size_t)i] < 0 && hasHat && i == hatPart) ? -1 : partModel[(size_t)i];
                    cp[(size_t)i].m = mats[(size_t)i];
                }
                a.cels.push_back(cp);
            }
            S.actions.push_back(a);
        }

        // report
        {
            std::string rep = "stock parts filled:";
            int filled = 0;
            for (int i = 0; i < np; i++)
                if (!mapLog[(size_t)i].empty())
                {
                    rep += "\n  part " + std::to_string(i) + " <- " + mapLog[(size_t)i];
                    filled++;
                }
            note(rep);
            for (const std::string &u : unmapped)
                note("  no stock part for '" + u + "': put on the torso (name joints or groups head, torso, upper arm L, hand R, thigh L ... or set 3DMM_MAP:<name>)");
            char b[96];
            snprintf(b, sizeof(b), "%d of %d stock parts have geometry, %zu actions", filled, np, S.actions.size());
            note(b);
        }

        // picker thumbnail
        if (!tryPreview(&S.thumb))
        {
            std::vector<RenderPart> rp;
            for (int i = 0; i < np; i++)
            {
                if (partModel[(size_t)i] == 0)
                    continue;
                const auto &cs = S.costumes[(size_t)S.parts[(size_t)i].bset][0];
                auto it = cs.find(i);
                rp.push_back(RenderPart{S.models[(size_t)partModel[(size_t)i]].get(), it != cs.end() ? it->second.texture.get() : nullptr});
            }
            if (hasHat)
            {
                const auto &cs = S.costumes[(size_t)S.parts[(size_t)hatPart].bset][0];
                rp.push_back(RenderPart{S.accModels[0].get(), cs.at(hatPart).texture.get()});
            }
            S.thumb = RenderThumb(rp);
        }
        return true;
    }

    // ---- parts
    Spec &S = *spec;
    S = Spec();
    S.name = opt.name;
    S.prop = opt.prop;
    S.rest[0] = 0;
    S.rest[1] = 0x4000; // stock actors are modelled facing -X and carry this 90 degree rest rotation
    S.rest[2] = 0;
    std::vector<int> topoOfPart;      // part -> topo piece (-1 = blank part)
    std::vector<int> partJoint;       // part -> joint it is the node of (-1 = none)
    std::vector<int> partOfTopo(topo.size(), -1);
    std::vector<int> jointPart(njoints, -1);
    {
        Part root;
        root.name = "root";
        root.parent = -1;
        root.bset = 0;
        S.parts.push_back(root);
        topoOfPart.push_back(-1);
        partJoint.push_back(-1);
        auto addPiece = [&](size_t ti, const std::string &name, int parent) {
            Part part;
            part.name = name;
            part.parent = parent;
            part.bset = denseSet(topo[ti].set);
            partOfTopo[ti] = (int)S.parts.size();
            S.parts.push_back(part);
            topoOfPart.push_back((int)ti);
            partJoint.push_back(-1);
        };
        if (!skeletal)
        {
            for (size_t ti = 0; ti < topo.size(); ti++)
                addPiece(ti, topo[ti].name, 0);
        }
        else
        {
            for (size_t ti = 0; ti < topo.size(); ti++)
                if (topo[ti].joint < 0)
                    addPiece(ti, topo[ti].name, 0);
            // joints, parents first; the joint's node part carries its first piece, further pieces hang off it
            std::function<void(int, int)> emit = [&](int j, int depth) {
                if (jointPart[j] >= 0 || depth > njoints)
                    return;
                int pj = model->getBoneJointParent((unsigned)j);
                int parentPart = 0;
                if (pj >= 0 && pj < njoints)
                {
                    emit(pj, depth + 1);
                    if (jointPart[pj] >= 0)
                        parentPart = jointPart[pj];
                }
                std::string jn = model->getBoneJointName((unsigned)j);
                if (jn.empty())
                    jn = "joint" + std::to_string(j);
                std::vector<size_t> mine;
                for (size_t ti = 0; ti < topo.size(); ti++)
                    if (topo[ti].joint == j)
                        mine.push_back(ti);
                int node;
                if (mine.empty())
                {
                    Part part;
                    part.name = jn;
                    part.parent = parentPart;
                    part.bset = 0;
                    node = (int)S.parts.size();
                    S.parts.push_back(part);
                    topoOfPart.push_back(-1);
                    partJoint.push_back(-1);
                }
                else
                {
                    addPiece(mine[0], jn, parentPart);
                    node = partOfTopo[mine[0]];
                }
                partJoint[node] = j;
                jointPart[j] = node;
                for (size_t k = 1; k < mine.size(); k++)
                    addPiece(mine[k], jn + "/" + topo[mine[k]].name, node);
            };
            for (int j = 0; j < njoints; j++)
                emit(j, 0);
        }
    }
    const int nparts = (int)S.parts.size();

    // ---- costumes
    S.costumes.assign(nsets, std::vector<std::map<int, Look>>());
    for (int s = 0; s < nsets; s++)
    {
        size_t ncost = 1;
        for (size_t i = 0; i < topo.size(); i++)
            if (S.parts[partOfTopo[i]].bset == s && topo[i].material >= 0)
            {
                auto it = variantsOf.find(topo[i].material);
                if (it != variantsOf.end())
                    ncost = std::max(ncost, it->second.size() + 1);
            }
        for (size_t k = 0; k < ncost; k++)
        {
            std::map<int, Look> costume;
            for (size_t i = 0; i < topo.size(); i++)
            {
                if (S.parts[partOfTopo[i]].bset != s)
                    continue;
                int mat = topo[i].material;
                if (mat >= 0 && k > 0)
                {
                    auto it = variantsOf.find(mat);
                    if (it != variantsOf.end() && k - 1 < it->second.size())
                        mat = it->second[k - 1];
                }
                Look look;
                look.texture = imageFor(mat);
                costume[partOfTopo[i]] = look;
            }
            S.costumes[s].push_back(costume);
        }
        if (ncost > 1)
        {
            char b[96];
            snprintf(b, sizeof(b), "body part set %d: %zu skins", setIds[s], ncost);
            note(b);
        }
    }

    // ---- poses -> models
    auto blank = std::make_shared<Mesh>();
    S.models.push_back(blank); // model 0: the empty model every stock root part carries

    std::map<std::string, int> modelIndex;
    auto modelFor = [&](size_t ti, const std::vector<std::array<float, 3>> &pos) -> int {
        const PartTopo &p = topo[ti];
        std::string key((const char *)&ti, sizeof(ti));
        for (unsigned sv : p.srcVertex)
            key.append((const char *)pos[sv].data(), 12);
        auto it = modelIndex.find(key);
        if (it != modelIndex.end())
            return it->second;
        auto m = std::make_shared<Mesh>();
        m->verts.resize(p.srcVertex.size());
        for (size_t i = 0; i < p.srcVertex.size(); i++)
        {
            const std::array<float, 3> &q = pos[p.srcVertex[i]];
            m->verts[i] = Vertex{q[0], q[1], q[2], p.u[i], p.v[i]};
        }
        m->faces = p.faces;
        int idx = (int)S.models.size();
        S.models.push_back(m);
        modelIndex[key] = idx;
        return idx;
    };
    // W[j] = where joint j's rigid body has moved to (3DMM space, row of the rest pose = identity); a part's matrix is
    // relative to its parent part:  M_j = W_parent^-1 o W_j, which telescopes back to W_j along the chain.
    std::vector<Aff> restW(njoints);
    std::vector<std::array<float, 3>> rest(nverts);
    for (unsigned v = 0; v < nverts; v++)
    {
        double c[3];
        model->getVertexCoordsUnanimated(v, c);
        xf.Apply(c, rest[v].data());
    }
    int fitted = 0, baked = 0;
    // G[p] = where part p's model ends up in the world; the cel matrix of a part is  G[parent]^-1 o G[p]  (it telescopes
    // back to G[p] along the parent chain).  Joint parts follow their joint; with fit = true every piece whose vertices
    // are an affine image of its rest shape keeps the rest model and gets that matrix, the others get a baked model.
    auto makeCel = [&](const std::vector<std::array<float, 3>> &pos, const std::vector<Aff> &W, bool fit) {
        std::vector<CelPart> cel(nparts);
        std::vector<Aff> G(nparts);
        cel[0].model = 0;
        for (int pi = 1; pi < nparts; pi++)
        {
            const int ti = topoOfPart[pi];
            const int j = partJoint[pi];
            Aff g;
            if (j >= 0)
                g = W[j];
            else if (S.parts[pi].parent > 0 && ti >= 0 && topo[ti].joint >= 0)
                g = G[S.parts[pi].parent]; // extra piece of a joint rides with the joint's node part
            bool useRest = true;
            if (ti >= 0 && fit)
            {
                const PartTopo &p = topo[ti];
                std::vector<std::array<double, 3>> P(p.srcVertex.size()), Q(p.srcVertex.size());
                bool same = true;
                for (size_t k = 0; k < p.srcVertex.size(); k++)
                    for (int i = 0; i < 3; i++)
                    {
                        P[k][i] = rest[p.srcVertex[k]][i];
                        Q[k][i] = pos[p.srcVertex[k]][i];
                        if (std::fabs(P[k][i] - Q[k][i]) > 1e-6)
                            same = false;
                    }
                Aff a;
                if (same)
                    g = Aff();
                else if (AffineFit(P, Q, opt.fitTol, &a))
                {
                    g = a;
                    fitted++;
                }
                else
                {
                    g = Aff();
                    useRest = false;
                    baked++;
                }
            }
            G[pi] = g;
            if (ti >= 0)
                cel[pi].model = modelFor((size_t)ti, useRest ? rest : pos);
            else
                cel[pi].model = 0;
            const int par = S.parts[pi].parent;
            cel[pi].m = ToMat34(par >= 0 ? Mul(Inv(G[par]), g) : g);
        }
        return cel;
    };

    {
        Action a;
        a.name = "At Rest";
        a.dwr = (float)opt.dwr;
        a.cels.push_back(makeCel(rest, restW, false));
        S.actions.push_back(a);
    }
    // animation names: "Walk*" or "Walk*1.2" marks a moving action (the actor travels while it plays, step length in
    // 3DMM units per cel); 3DMM_STEP:<name> does the same from the metadata
    auto parseName = [&](const std::string &raw, Action *a) {
        std::string nm = raw;
        double dist = 0;
        bool moving = false;
        size_t star = nm.find('*');
        if (star != std::string::npos)
        {
            moving = true;
            std::string num = nm.substr(star + 1);
            if (!num.empty() && atof(num.c_str()) > 0)
                dist = atof(num.c_str());
            nm = nm.substr(0, star);
        }
        while (!nm.empty() && nm.back() == ' ')
            nm.pop_back();
        std::string mv = Meta(model, ("3DMM_STEP:" + nm).c_str());
        if (!mv.empty() && atof(mv.c_str()) > 0)
        {
            moving = true;
            dist = atof(mv.c_str());
        }
        a->moving = moving;
        a->name = moving ? nm + " *" : nm;
        a->dwr = (float)(moving ? (dist > 0 ? dist : opt.step) : opt.dwr);
    };
    if (!opt.skipAnims)
    {
        unsigned nanim = model->getAnimCount(Model::ANIMMODE_FRAME);
        for (unsigned an = 0; an < nanim; an++)
        {
            unsigned nf = model->getAnimFrameCount(Model::ANIMMODE_FRAME, an);
            if (nf == 0)
                continue;
            Action a;
            parseName(model->getAnimName(Model::ANIMMODE_FRAME, an), &a);
            for (unsigned f = 0; f < nf; f++)
            {
                std::vector<std::array<float, 3>> pos(nverts);
                for (unsigned v = 0; v < nverts; v++)
                {
                    double c[3];
                    if (!model->getFrameAnimVertexCoords(an, f, v, c[0], c[1], c[2]))
                        model->getVertexCoordsUnanimated(v, c);
                    xf.Apply(c, pos[v].data());
                }
                a.cels.push_back(makeCel(pos, restW, opt.fit));
            }
            S.actions.push_back(a);
        }
        if (nanim > 0)
        {
            char b[128];
            snprintf(b, sizeof(b), "%u frame animations, %zu distinct models (%d parts-in-frames fitted by matrix)", nanim, S.models.size(), fitted);
            note(b);
        }
    }
    if (!opt.skipAnims && skeletal)
    {
        const Aff A = AffOf([&](const double *p, double *o) {
            double r[3];
            xf.Rot(p, r);
            for (int i = 0; i < 3; i++)
                o[i] = r[i] * xf.s + xf.t[i];
        });
        const Aff Ai = Inv(A);
        unsigned nanim = model->getAnimCount(Model::ANIMMODE_SKELETAL);
        unsigned made = 0;
        for (unsigned an = 0; an < nanim; an++)
        {
            unsigned nf = model->getAnimFrameCount(Model::ANIMMODE_SKELETAL, an);
            if (nf == 0)
                continue;
            if (!model->setCurrentAnimation(Model::ANIMMODE_SKELETAL, an))
                continue;
            Action a;
            parseName(model->getAnimName(Model::ANIMMODE_SKELETAL, an), &a);
            for (unsigned f = 0; f < nf; f++)
            {
                model->setCurrentAnimationFrame(f);
                std::vector<Aff> W(njoints);
                for (int j = 0; j < njoints; j++)
                {
                    // MM3D skins a vertex as  v * inverse(absolute rest matrix) * final matrix
                    Matrix fm, am;
                    if (!model->getBoneJointFinalMatrix((unsigned)j, fm) || !model->getBoneJointAbsoluteMatrix((unsigned)j, am))
                        continue;
                    const Matrix ami = am.getInverse();
                    Aff F = AffOf([&](const double *p, double *o) {
                        o[0] = p[0];
                        o[1] = p[1];
                        o[2] = p[2];
                        ami.apply3x(o);
                        fm.apply3x(o);
                    });
                    W[j] = Mul(A, Mul(F, Ai));
                }
                if (opt.skinBake)
                {
                    std::vector<std::array<float, 3>> pos(nverts);
                    for (unsigned v = 0; v < nverts; v++)
                    {
                        double c[3];
                        model->getVertexCoords(v, c);
                        xf.Apply(c, pos[v].data());
                    }
                    a.cels.push_back(makeCel(pos, restW, true));
                }
                else
                    a.cels.push_back(makeCel(rest, W, false));
            }
            S.actions.push_back(a);
            made++;
        }
        model->setNoAnimation();
        if (made > 0)
        {
            char b[96];
            snprintf(b, sizeof(b), "%u skeletal animations over %d joints%s", made, njoints, opt.skinBake ? " (skin baked)" : "");
            note(b);
        }
    }
    // a user animation called "At Rest" replaces the generated rest pose (many models are modelled in a T-pose)
    {
        int found = -1;
        for (size_t i = 1; i < S.actions.size() && found < 0; i++)
            if (strcasecmp(S.actions[i].name.c_str(), "At Rest") == 0)
                found = (int)i;
        if (found > 0)
        {
            Action a = S.actions[(size_t)found];
            a.name = "At Rest";
            a.moving = false;
            S.actions[0] = a;
            note("your 'At Rest' animation replaces the default rest pose");
        }
        for (size_t i = S.actions.size(); i-- > 1;)
            if (strcasecmp(S.actions[i].name.c_str(), "At Rest") == 0)
                S.actions.erase(S.actions.begin() + (long)i); // no duplicate "At Rest" actions
    }
    if (opt.propAnims)
    {
        // The stock props' basic transforms, applied to the root part (every other part hangs off it, so the
        // whole model follows).  Numbers are taken from the stock Cube prop: 24 cels of 15% steps etc.
        struct Def
        {
            const char *name;
            int cels;
            int kind; // 0 grow, 1 shrink, 2 spin, 3 squash, 4 stretch
            double step;
        };
        static const Def defs[] = {{"Grow", 24, 0, 0.15},     {"Grow Fast", 14, 0, 0.30}, {"Shrink", 24, 1, 0.0375},
                                   {"Shrink Fast", 14, 1, 0.075}, {"Spin", 12, 2, 30.0},     {"Spin Fast", 6, 2, 60.0},
                                   {"Squash", 24, 3, 0.15},   {"Stretch", 24, 4, 0.15}};
        const std::vector<CelPart> restCel = S.actions[0].cels[0];
        int added = 0;
        for (const Def &d : defs)
        {
            bool dup = false;
            for (const Action &x : S.actions)
                if (strcasecmp(x.name.c_str(), d.name) == 0)
                    dup = true;
            if (dup)
                continue;
            Action a;
            a.name = d.name;
            a.dwr = (float)opt.dwr;
            for (int i = 0; i < d.cels; i++)
            {
                double up = std::min(4.0, (d.kind == 0 && d.step > 0.2 ? 1.3 : 1.15) + d.step * i);   // 1.15 + 0.15 i (Fast: 1.3 + 0.3 i)
                double dn = std::max(0.25, 1.0 - d.step * (i + 1));                                      // 1 - 0.0375 (i+1)
                if (d.kind == 3 || d.kind == 4)
                    dn = std::max(0.25, 1.0 - 0.0375 * (i + 1));
                double sx = 1, sy = 1, sz = 1, ang = 0;
                switch (d.kind)
                {
                case 0: sx = sy = sz = up; break;
                case 1: sx = sy = sz = dn; break;
                case 2: ang = d.step * (i + 1) * M_PI / 180.0; break;
                case 3: sx = sz = up; sy = dn; break;
                case 4: sy = up; sx = sz = dn; break;
                }
                std::vector<CelPart> cel = restCel;
                Mat34 &r = cel[0].m;
                r.r[0][0] = (float)(sx * cos(ang));
                r.r[0][2] = (float)(-sx * sin(ang));
                r.r[1][1] = (float)sy;
                r.r[2][0] = (float)(sz * sin(ang));
                r.r[2][2] = (float)(sz * cos(ang));
                a.cels.push_back(cel);
            }
            S.actions.push_back(a);
            added++;
        }
        char b[64];
        snprintf(b, sizeof(b), "%d stock prop animations appended", added);
        note(b);
    }
    if (S.models.size() > 32000)
        return fail("too many distinct models (more than 32000) - reduce frames or parts");

    // ---- picker thumbnail
    bool havePreview = tryPreview(&S.thumb);
    if (!havePreview)
    {
        std::vector<RenderPart> rp;
        for (size_t ti = 0; ti < topo.size(); ti++)
        {
            const int pi = partOfTopo[ti];
            const CelPart &cp = S.actions[0].cels[0][pi];
            rp.push_back(RenderPart{S.models[cp.model].get(), S.costumes[S.parts[pi].bset][0][pi].texture.get()});
        }
        S.thumb = RenderThumb(rp);
    }
    return true;
}
