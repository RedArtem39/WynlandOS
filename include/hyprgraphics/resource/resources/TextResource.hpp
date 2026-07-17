#ifndef HYPRGRAPHICS_RESOURCE_RESOURCES_TEXT_RESOURCE_HPP
#define HYPRGRAPHICS_RESOURCE_RESOURCES_TEXT_RESOURCE_HPP

#include <string>
#include "../../color/Color.hpp"
#include "../Resource.hpp"
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprgraphics {

struct STextResourceData {
    std::string text = "";
    std::string font = "Sans";
    double      fontSize = 12.0;
    CColor      color = CColor(1, 1, 1, 1);
    Hyprutils::Math::Vector2D maxSize = {-1, -1};
    std::string fontFamily = "Sans";
    bool        ellipsize = false;
    bool        wrap = false;
};

class CTextResource : public IResource {
public:
    using STextResourceData = Hyprgraphics::STextResourceData;
    CTextResource() {}
    virtual ~CTextResource() = default;
};

namespace Resource {
    using CTextResource = Hyprgraphics::CTextResource;
    using STextResourceData = Hyprgraphics::STextResourceData;
}

}

#endif /* HYPRGRAPHICS_RESOURCE_RESOURCES_TEXT_RESOURCE_HPP */
