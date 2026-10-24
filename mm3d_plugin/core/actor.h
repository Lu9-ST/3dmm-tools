// 3DMM actor / prop (TMPL) description and writer.  Port of tools/actorbuild.py.
#ifndef DMM_ACTOR_H
#define DMM_ACTOR_H

#include "chunky.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dmm
{

// A texture page as palette indices (10..155 of the shared actor palette), rows top-down.
struct Image8
{
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};
typedef std::shared_ptr<const Image8> ImagePtr;

struct Vertex
{
    float x, y, z, u, v; // 3DMM units; uv in 0..1 with v = 0 at the bottom of the texture
};

struct Mesh
{
    std::vector<Vertex> verts;
    std::vector<std::array<uint16_t, 3>> faces; // counter-clockwise seen from outside
};
typedef std::shared_ptr<const Mesh> MeshPtr;

// Row-vector convention, relative to the parent part: p' = p * R + t
struct Mat34
{
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float t[3] = {0, 0, 0};
};

struct Part
{
    std::string name;
    int parent = -1; // index of the parent part, -1 = root
    int bset = 0;    // body part set (costumes are chosen per set)
};

// What one part of a set looks like in one costume.
struct Look
{
    ImagePtr texture;  // null = solid colour
    int solidBase = 15; // palette ramp base for solid colour
    int accModel = -1; // accessory costumes: index into Spec::accModels (-1 = none, material only)
};

struct CelPart
{
    int model = -1; // index into Spec::models, -1 = leave the part's model alone (accessories)
    Mat34 m;
};

struct Action
{
    std::string name;
    std::vector<std::vector<CelPart>> cels; // [cel][part]
    float dwr = 2.5f;                       // distance travelled per cel, in 3DMM units (moving actions: the step length)
    bool moving = false;                    // false: stationary action; true: the actor travels along its path while it plays
    int64_t flagsRaw = -1;                  // >= 0: ACTN flags copied verbatim (stock actions); overrides `moving`
    std::vector<std::array<uint32_t, 2>> celFixed; // optional raw GGCL fixed data per cel (flags, distance per cel)
};

struct Spec
{
    std::string name;
    bool prop = false;
    int16_t rest[3] = {0, 0, 0}; // rest rotation, BRA (0x4000 = 90 degrees)
    std::vector<Part> parts;
    std::vector<MeshPtr> models;    // BMDL chid under the TMPL = index
    std::vector<MeshPtr> accModels; // models owned by accessory costumes
    // costumes[bset] = list of costumes; each costume maps part index -> Look.  First costume is the default.
    std::vector<std::vector<std::map<int, Look>>> costumes;
    std::vector<Action> actions; // first should be "At Rest"
    Image8 thumb;                // 72x72, palette indices
};

struct BuildResult
{
    Bytes cn3; // .3cn
    Bytes th3; // .3th
};

bool BuildActor(const Spec &spec, uint32_t cnoBase, BuildResult *out, std::string *err);

// Writes a VXP (zip) containing <base>.3cn, <base>.3th and <base>.cfg.
bool BuildVxp(const Spec &spec, const std::string &base, uint32_t cnoBase, const std::string &title,
              const std::string &author, Bytes *vxp, std::string *err);

// Low level encoders, exposed for tests.
Bytes EncBmdl(const Mesh &m);
Bytes EncTmap(const Image8 &img);
Bytes EncMbmp(const Image8 &img);

} // namespace dmm

#endif
