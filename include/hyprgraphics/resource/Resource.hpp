#ifndef HYPRGRAPHICS_RESOURCE_RESOURCE_HPP
#define HYPRGRAPHICS_RESOURCE_RESOURCE_HPP

#include <string>
#include <hyprutils/math/Vector2D.hpp>
#include "../color/Color.hpp"

namespace Hyprgraphics {

using CHyprColor = CColor;

namespace Resource {
    class IResource {
    public:
        virtual ~IResource() = default;
    };
}

using Resource::IResource;

class CTextResource;
class CImageResource;

}

#include "resources/TextResource.hpp"
#include "resources/ImageResource.hpp"

#endif /* HYPRGRAPHICS_RESOURCE_RESOURCE_HPP */
