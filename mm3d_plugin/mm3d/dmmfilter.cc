#include "dmmfilter.h"

#include "dmmexport.h"

#include "../core/actor.h"
#include "../core/zipwriter.h"

#include "datadest.h"
#include "filtermgr.h"
#include "pluginapi.h"
#include "log.h"
#include "model.h"
#include "version.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#ifndef DMM_PLUGIN_VERSION_STRING
#define DMM_PLUGIN_VERSION_STRING VERSION_STRING
#endif

static DmmFilter *s_filter = NULL;

DmmFilter::DmmFilter()
{
}

DmmFilter::~DmmFilter()
{
}

Model::ModelErrorE DmmFilter::readFile(Model *, const char *const)
{
    return Model::ERROR_UNSUPPORTED_OPERATION;
}

bool DmmFilter::isSupported(const char *file)
{
    size_t len = file ? strlen(file) : 0;
    return len >= 4 && strcasecmp(&file[len - 4], ".vxp") == 0;
}

std::list<std::string> DmmFilter::getReadTypes()
{
    return std::list<std::string>();
}

std::list<std::string> DmmFilter::getWriteTypes()
{
    std::list<std::string> r;
    r.push_back("*.vxp");
    return r;
}

Model::ModelErrorE DmmFilter::writeFile(Model *model, const char *const filename, ModelFilter::Options *)
{
    if (!model || !filename || !filename[0])
        return Model::ERROR_BAD_ARGUMENT;

    DmmOptions opt = DmmOptionsFromModel(model, filename);
    dmm::Spec spec;
    std::string err, logText;
    if (!DmmBuildSpec(model, opt, &spec, &err, &logText))
    {
        model->setFilterSpecificError(("3DMM export: " + err).c_str());
        return Model::ERROR_FILTER_SPECIFIC;
    }
    fprintf(stderr, "[3DMM export] %s", logText.c_str());

    uint32_t cno = opt.cno;
    if (cno == 0)
        cno = 0x20000000u + (dmm::Crc32((const uint8_t *)opt.name.data(), opt.name.size()) & 0x0FFFFF00u);

    dmm::Bytes vxp;
    if (!dmm::BuildVxp(spec, opt.base, cno, opt.name, opt.author, &vxp, &err))
    {
        model->setFilterSpecificError(("3DMM export: " + err).c_str());
        return Model::ERROR_FILTER_SPECIFIC;
    }

    Model::ModelErrorE e = Model::ERROR_NONE;
    DataDest *dst = openOutput(filename, e);
    if (e != Model::ERROR_NONE || !dst)
        return e != Model::ERROR_NONE ? e : Model::ERROR_FILE_OPEN;
    DestCloser closer(dst);
    if (!dst->writeBytes(vxp.data(), vxp.size()))
        return Model::ERROR_FILE_OPEN;
    return Model::ERROR_NONE;
}

#ifdef PLUGIN

PLUGIN_API bool plugin_init()
{
    if (s_filter == NULL)
    {
        s_filter = new DmmFilter();
        FilterManager *mgr = FilterManager::getInstance();
        mgr->registerFilter(s_filter);
    }
    log_debug("3DMM actor exporter initialized\n");
    return true;
}

// The filter manager deletes registered filters.
PLUGIN_API bool plugin_uninit()
{
    s_filter = NULL;
    return true;
}

PLUGIN_API const char *plugin_mm3d_version()
{
    return DMM_PLUGIN_VERSION_STRING;
}

PLUGIN_API const char *plugin_version()
{
    return "0.6.0";
}

PLUGIN_API const char *plugin_desc()
{
    return "3D Movie Maker actor/prop exporter (.vxp)";
}

#endif // PLUGIN
