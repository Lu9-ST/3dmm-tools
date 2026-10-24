// Model -> dmm::Spec mapping (everything that needs the MM3D headers lives in dmmexport.cc)
#ifndef DMMEXPORT_H
#define DMMEXPORT_H

#include "model.h"

#include "../core/actor.h"

#include <string>
#include <vector>

struct DmmOptions
{
    std::string name;      // title in the picker / cfg
    std::string author;
    std::string base;      // file base name inside the VXP
    bool prop = false;
    std::string forward = "+Z"; // which MM3D axis is the front of the model (+Z is what the MD2 importer produces)
    double scale = 0;      // >0: explicit multiplier (disables auto-fit)
    double fitHeight = 10; // auto-fit the model to this height (3DMM units) when scale is not given; <=0 keeps scale 1
    bool feetOrigin = true; // move the model so its feet are on y = 0 and it is centred on x/z
    bool dither = false;
    int texMax = 0;         // max texture width/height; 0 = keep the original size
    bool texPow2 = false;   // round texture sizes to a power of two
    uint32_t cno = 0;      // 0 = derive from the name
    std::string preview;   // optional image file for the picker thumbnail
    double dwr = 2.5;      // distance per cel of the animations
    bool skipAnims = false;
    double motionScale = 0;    // 3DMM_MOTION_SCALE: default mode, scale of jumps/slides; 0 = user height / stock height
    bool fitHeightSet = false; // 3DMM_HEIGHT was given
    bool actorAnims = false;   // default actor mode: reuse a stock actor's body parts and animations
    std::string stock;         // 3DMM_STOCK: tmpls.3cn (or the folder holding it)
    std::string stockActor = "Frank";
    std::string modelDir;      // folder of the model file (for finding things next to it)
    bool fit = true;        // frame animations: a part that merely moved/rotated/scaled keeps its rest model + a matrix
    double fitTol = 0.01;   // max vertex error (3DMM units) accepted by that fit
    bool skinBake = false;  // skeletons: store the weighted (blended) vertex positions per frame instead of rigid joint motion
    bool skeleton = true;   // joints become rigid parts (one per joint) and skeletal animations become cels
    bool propAnims = false; // append the stock prop animations (Grow, Shrink, Spin, Squash, Stretch ...)
    double step = 1.0;      // default step length (3DMM units per cel) of animations marked as moving
};

// Reads the options from the model's metadata (3DMM_* keys) and the output file name.
DmmOptions DmmOptionsFromModel(Model *model, const char *outFilename);

// Builds the actor description; on failure returns false and sets *err.
bool DmmBuildSpec(Model *model, const DmmOptions &opt, dmm::Spec *spec, std::string *err, std::string *log);

#endif
