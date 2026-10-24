// Reads a stock 3DMM actor template (TMPL: part tree + body part sets + all its actions) out of the user's tmpls.3cn,
// so that default-actor exports can reuse its animations.
#ifndef DMM_STOCK_H
#define DMM_STOCK_H

#include "actor.h"
#include "geom.h"

namespace dmm
{

struct StockCel
{
    uint32_t fixed[2] = {0, 0};  // raw GGCL fixed part (flags, distance per cel)
    std::vector<int> model;      // per part: BMDL chid of the stock model (-1 = accessory slot)
    std::vector<Mat34> m;        // per part, relative to the parent part
};

struct StockAction
{
    std::string name;
    uint32_t flags = 0; // ACTN data
    std::vector<StockCel> cels;
};

struct StockActor
{
    std::string name;
    int16_t rest[3] = {0, 0, 0};
    std::vector<int> parent, bset;
    std::vector<StockAction> actions;
    double restMin[3] = {0, 0, 0}, restMax[3] = {0, 0, 0}; // world bounding box of the models in the first cel of the first action

    size_t Parts() const
    {
        return parent.size();
    }
    // world transform of every part in one cel (p_world = M_root o ... o M_part)
    std::vector<Aff> World(const StockCel &c) const;
};

// Matrices that make a model built in rest-pose world coordinates follow the stock animation.  For a part with a
// parent the matrix is the stock part's motion relative to its parent, turned around the user's own pivot
// (userPivot[i], valid when hasPivot[i]) instead of the stock one; root parts get the stock world motion.  In Frank's
// 27-part layout the head, torso, hips and shoes are tied together through a virtual tree so they stay attached when the
// proportions differ; motionScale (user height / stock height) scales jumps and other slides.
std::vector<Mat34> RetargetCel(const StockActor &s, const std::vector<Aff> &rest, const StockCel &cel,
                               const std::vector<std::array<double, 3>> &userPivot, const std::vector<bool> &hasPivot,
                               double motionScale = 1.0);

bool LoadStockActor(const std::string &path, const std::string &actorName, StockActor *out, std::string *err);

// Looks for tmpls.3cn: explicit path (file or folder), then next to the model, then the usual install places.
std::string FindStockFile(const std::string &hint, const std::string &modelDir);

} // namespace dmm

#endif
