#pragma once
#include <RmlUi/Core/PropertyParser.h>
#include "RmlGrid.h"

namespace Rml {
class GridPropertyParser final : public PropertyParser {
public:
    explicit GridPropertyParser(const char* kind) : kind(kind) {}
    bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override
    {
        if (String(kind) == "alignment" && value == "auto" && parameters.find("auto") == parameters.end()) return false;
        if (!RmlGrid_Validate(kind, value.c_str())) return false;
        property.value = value;
        property.unit = Unit::STRING;
        return true;
    }
private:
    const char* kind;
};
}
