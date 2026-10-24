// Export filter: Maverick Model 3D -> 3D Movie Maker actor/prop (.vxp)
#ifndef DMMFILTER_H
#define DMMFILTER_H

#include "modelfilter.h"

#include <list>
#include <string>

class DmmFilter : public ModelFilter
{
  public:
    DmmFilter();
    virtual ~DmmFilter();

    Model::ModelErrorE readFile(Model *model, const char *const filename);
    Model::ModelErrorE writeFile(Model *model, const char *const filename, ModelFilter::Options *o = NULL);

    bool canRead(const char *filename = NULL)
    {
        return false;
    }
    bool canWrite(const char *filename = NULL)
    {
        return false;
    }
    bool canExport(const char *filename = NULL)
    {
        return true;
    }

    bool isSupported(const char *file);

    std::list<std::string> getReadTypes();
    std::list<std::string> getWriteTypes();
};

#endif
