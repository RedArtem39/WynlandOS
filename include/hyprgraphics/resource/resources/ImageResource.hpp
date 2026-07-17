#ifndef HYPRGRAPHICS_RESOURCE_RESOURCES_IMAGE_RESOURCE_HPP
#define HYPRGRAPHICS_RESOURCE_RESOURCES_IMAGE_RESOURCE_HPP

#include "../Resource.hpp"

namespace Hyprgraphics {

class CImageResource : public IResource {
public:
    CImageResource() {}
    virtual ~CImageResource() = default;
};

namespace Resource {
    using CImageResource = Hyprgraphics::CImageResource;
}

}

#endif /* HYPRGRAPHICS_RESOURCE_RESOURCES_IMAGE_RESOURCE_HPP */
