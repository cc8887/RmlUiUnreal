#pragma once
#include <RmlUi/Core/Types.h>
namespace Rml {
class ContainerBox;
class LayoutBox;
class Box;
class GridFormattingContext {
public:
    static UniquePtr<LayoutBox> Format(ContainerBox* parent, Element* element, const Box* override_box);
    static float ShrinkToFit(Element* element, Vector2f containing);
};
}
