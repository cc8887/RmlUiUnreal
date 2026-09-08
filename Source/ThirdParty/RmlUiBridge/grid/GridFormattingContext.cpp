#include "GridFormattingContext.h"
#include "RmlGrid.h"
#include "Layout/ContainerBox.h"
#include "Layout/FormattingContext.h"
#include "Layout/LayoutDetails.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementScroll.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Core.h>
#include <algorithm>
#include <array>
#include <cfloat>

namespace Rml {
namespace {
thread_local float intrinsic_width = 0;
struct IntrinsicScope {
    float previous;
    explicit IntrinsicScope(float mode) : previous(intrinsic_width) { intrinsic_width = mode; }
    ~IntrinsicScope() { intrinsic_width = previous; }
};
class GridContainer final : public ContainerBox {
public:
    GridContainer(Element* element, ContainerBox* parent) : ContainerBox(Type::GridContainer, element, parent) {}
    Box box;
    const Box* GetIfBox() const override { return &box; }
    float GetShrinkToFitWidth() const override { return box.GetSize().x; }
    String DebugDumpTree(int depth) const override { return String(depth * 2, ' ') + "GridContainer"; }
    bool Close(Vector2f visible, Vector2f scrollable, const Box& result, float baseline)
    {
        box = result;
        if (!SubmitBox(visible, scrollable, box, -1)) return false;
        ClosePositionedElements();
        SubmitElementLayout();
        SetElementBaseline(baseline);
        return true;
    }
};
RmlGridLength Length(Style::LengthPercentageAuto value) { return {value.value, int(value.type)}; }
RmlGridLength Length(Style::LengthPercentage value) { return {value.value, value.type == Style::LengthPercentage::Percentage ? 2 : 1}; }
RmlGridLength Pixels(float value) { return {value, 1}; }
struct OwnedStyle {
    RmlGridStyle value{};
    std::array<String, 16> strings;
    size_t next = 0;
    const char* PropertyString(Element* element, PropertyId id, const char* fallback)
    {
        String& result = strings[next++];
        const Property* property = element->GetProperty(id);
        result = property ? property->ToString() : String(fallback);
        return result.c_str();
    }
    void Initialize(Element* element, bool container)
    {
        const ComputedValues& c = element->GetComputedValues();
        value.size[0] = Length(c.width()); value.size[1] = Length(c.height());
        value.min_size[0] = Length(c.min_width()); value.min_size[1] = Length(c.min_height());
        value.max_size[0] = c.max_width().value >= FLT_MAX ? RmlGridLength{} : Length(c.max_width());
        value.max_size[1] = c.max_height().value >= FLT_MAX ? RmlGridLength{} : Length(c.max_height());
        // CSS grid has an automatic minimum size; RmlUi's initial min-width/height is zero.
        for (int axis = 0; axis < 2; ++axis) {
            const PropertyId id = axis == 0 ? PropertyId::MinWidth : PropertyId::MinHeight;
            if (!element->GetLocalProperty(id)) value.min_size[axis] = {};
        }
        value.margin[0] = Length(c.margin_top()); value.margin[1] = Length(c.margin_right());
        value.margin[2] = Length(c.margin_bottom()); value.margin[3] = Length(c.margin_left());
        value.padding[0] = Length(c.padding_top()); value.padding[1] = Length(c.padding_right());
        value.padding[2] = Length(c.padding_bottom()); value.padding[3] = Length(c.padding_left());
        value.border[0] = c.border_top_width(); value.border[1] = c.border_right_width();
        value.border[2] = c.border_bottom_width(); value.border[3] = c.border_left_width();
        value.border_box = c.box_sizing() == Style::BoxSizing::BorderBox;
        value.overflow_x = c.overflow_x() != Style::Overflow::Visible;
        value.overflow_y = c.overflow_y() != Style::Overflow::Visible;
        if (container) {
            const Unit units[] = {Unit::EM, Unit::REM, Unit::DP, Unit::VW, Unit::VH};
            for (int i = 0; i < 5; ++i) value.units[i] = element->ResolveNumericValue({1, units[i]}, 0);
            value.columns = PropertyString(element, PropertyId::GridTemplateColumns, "none");
            value.rows = PropertyString(element, PropertyId::GridTemplateRows, "none");
            value.auto_columns = PropertyString(element, PropertyId::GridAutoColumns, "auto");
            value.auto_rows = PropertyString(element, PropertyId::GridAutoRows, "auto");
            value.areas = PropertyString(element, PropertyId::GridTemplateAreas, "none");
            value.flow = PropertyString(element, PropertyId::GridAutoFlow, "row");
            value.justify_items = PropertyString(element, PropertyId::JustifyItems, "stretch");
            value.align_items = PropertyString(element, PropertyId::AlignItems, "stretch");
            value.justify_content = element->GetLocalProperty(PropertyId::JustifyContent) ? PropertyString(element, PropertyId::JustifyContent, "stretch") : "stretch";
            value.align_content = PropertyString(element, PropertyId::AlignContent, "stretch");
            value.gap[0] = Length(c.column_gap()); value.gap[1] = Length(c.row_gap());
        } else {
            value.column_start = PropertyString(element, PropertyId::GridColumnStart, "auto");
            value.column_end = PropertyString(element, PropertyId::GridColumnEnd, "auto");
            value.row_start = PropertyString(element, PropertyId::GridRowStart, "auto");
            value.row_end = PropertyString(element, PropertyId::GridRowEnd, "auto");
            value.justify_self = PropertyString(element, PropertyId::JustifySelf, "auto");
            value.align_self = PropertyString(element, PropertyId::AlignSelf, "auto");
        }
    }
};
struct MeasureContext { ElementList items; };
Vector2f TextSize(ElementText* text, float width, bool commit)
{
    if (commit) text->ClearLines();
    Vector2f size;
    int begin = 0;
    const float line_height = text->GetLineHeight();
    const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(text->GetFontFaceHandle());
    while (begin < int(text->GetText().size())) {
        String line; int consumed = 0; float line_width = 0;
        text->GenerateLine(line, consumed, line_width, begin, width, 0, true, true, false);
        if (consumed <= 0) break;
        if (commit) text->AddLine({0, size.y + (line_height - metrics.line_spacing) * 0.5f + metrics.ascent}, line);
        size.x = Math::Max(size.x, line_width); size.y += line_height; begin += consumed;
    }
    return size;
}
void Measure(void* user, size_t index, float known_width, float known_height, float available_width, float available_height, float* width, float* height)
{
    Element* element = static_cast<MeasureContext*>(user)->items[index];
    float constraint = known_width >= 0 ? known_width : available_width >= 0 ? available_width : available_width == -1 ? 0.f : 100000.f;
    if (auto* text = rmlui_dynamic_cast<ElementText*>(element)) {
        const Vector2f size = TextSize(text, constraint, false); *width = size.x; *height = size.y; return;
    }
    const Vector2f containing(constraint, known_height >= 0 ? known_height : Math::Max(0.f, available_height));
    RootBox root(containing);
    Box box;
    LayoutDetails::BuildBox(box, containing, element, BuildBoxMode::UnalignedBlock);
    Vector2f size = box.GetSize();
    if (known_width >= 0) size.x = known_width;
    else if (!element->IsReplaced()) size.x = constraint;
    if (known_height >= 0) size.y = known_height;
    box.SetContent(size);
    const bool is_grid = element->GetDisplay() == Style::Display::Grid || element->GetDisplay() == Style::Display::InlineGrid;
    IntrinsicScope scope(is_grid && known_width < 0 && available_width < 0 ? available_width : 0);
    auto formatted = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block);
    if (!formatted || !formatted->GetIfBox()) { *width = *height = 0; return; }
    // Flex/table layout boxes can retain their initial auto size; the element owns the committed size.
    const Vector2f result = element->GetBox().GetSize();
    *width = known_width >= 0 || element->IsReplaced() || is_grid ? result.x : formatted->GetShrinkToFitWidth();
    *height = known_height >= 0 ? known_height : result.y;
}
}

float GridFormattingContext::ShrinkToFit(Element* element, Vector2f containing)
{
    RootBox parent(containing);
    Box box;
    LayoutDetails::BuildBox(box, containing, element, BuildBoxMode::UnalignedBlock);
    box.SetContent({0, box.GetSize().y});
    float widths[2] = {};
    for (int i = 0; i < 2; ++i) {
        IntrinsicScope scope(float(-1 - i));
        widths[i] = Format(&parent, element, &box)->GetShrinkToFitWidth();
    }
    const float available = containing.x - box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Padding);
    return Math::Min(Math::Max(widths[0], available), widths[1]);
}

UniquePtr<LayoutBox> GridFormattingContext::Format(ContainerBox* parent, Element* element, const Box* override_box)
{
    const float measure_mode = intrinsic_width;
    IntrinsicScope no_inherited_measure(0);
    auto container = MakeUnique<GridContainer>(element, parent);
    const Vector2f containing = LayoutDetails::GetContainingBlock(parent, element->GetPosition()).size;
    Box& box = container->box;
    if (override_box) box = *override_box;
    else LayoutDetails::BuildBox(box, containing, element);
    container->ResetScrollbars(box);
    const Vector2f initial_size = box.GetSize();
    float min_height = 0, max_height = FLT_MAX;
    LayoutDetails::GetMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing.y);
    MeasureContext context;
    for (int i = 0; i < element->GetNumChildren(); ++i) {
        Element* child = element->GetChild(i);
        if (child->GetDisplay() == Style::Display::None) continue;
        if (auto* text = rmlui_dynamic_cast<ElementText*>(child)) {
            if (StringUtilities::StripWhitespace(text->GetText()).empty()) { text->ClearLines(); text->SetBox(Box({0, 0})); continue; }
        }
        if (child->GetPosition() == Style::Position::Absolute || child->GetPosition() == Style::Position::Fixed) {
            LayoutDetails::GetContainingBlock(container.get(), child->GetPosition()).container->AddAbsoluteElement(child, {}, element);
            continue;
        }
        if (child->GetPosition() == Style::Position::Relative) container->AddRelativeElement(child);
        context.items.push_back(child);
    }
    std::stable_sort(context.items.begin(), context.items.end(), [](Element* a, Element* b) {
        return a->GetProperty(PropertyId::Order)->Get<float>() < b->GetProperty(PropertyId::Order)->Get<float>();
    });
    Vector<OwnedStyle> owned(context.items.size());
    Vector<RmlGridStyle> styles(context.items.size());
    for (size_t i = 0; i < styles.size(); ++i) { owned[i].Initialize(context.items[i], false); styles[i] = owned[i].value; }
    OwnedStyle root; root.Initialize(element, true);
    root.value.border_box = 0;
    for (int i = 0; i < 4; ++i) { root.value.padding[i] = Pixels(0); root.value.margin[i] = Pixels(0); root.value.border[i] = 0; }
    root.value.min_size[0] = Pixels(0); root.value.min_size[1] = Pixels(min_height);
    root.value.max_size[0] = {}; root.value.max_size[1] = max_height < FLT_MAX ? Pixels(max_height) : RmlGridLength{};
    Vector<RmlGridRect> rectangles(styles.size());
    for (int iteration = 0; iteration < 3; ++iteration) {
        const Vector2f scrollbar(element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL), element->GetElementScroll()->GetScrollbarSize(ElementScroll::HORIZONTAL));
        const Vector2f available = Math::Max(Vector2f(0), initial_size - scrollbar);
        root.value.size[0] = measure_mode < 0 ? RmlGridLength{measure_mode, 0} : Pixels(available.x);
        root.value.size[1] = initial_size.y < 0 ? RmlGridLength{} : Pixels(available.y);
        float result_width = 0, result_height = 0;
        char error[1024] = {};
        if (!RmlGrid_Layout(&root.value, styles.data(), styles.size(), Measure, &context, rectangles.data(), &result_width, &result_height, error, sizeof(error))) {
            Log::Message(Log::LT_ERROR, "Grid layout failed for %s: %s", element->GetAddress().c_str(), error);
            break;
        }
        Vector2f visible, scrollable;
        float baseline = 0;
        for (size_t i = 0; i < styles.size(); ++i) {
            Element* child = context.items[i]; const auto& rect = rectangles[i];
            Box child_box;
            LayoutDetails::BuildBox(child_box, {rect.width, rect.height}, child, BuildBoxMode::UnalignedBlock);
            for (int edge = 0; edge < 4; ++edge) {
                const BoxEdge edges[] = {BoxEdge::Top, BoxEdge::Right, BoxEdge::Bottom, BoxEdge::Left};
                child_box.SetEdge(BoxArea::Padding, edges[edge], rect.padding[edge]);
                child_box.SetEdge(BoxArea::Margin, edges[edge], rect.margin[edge]);
            }
            Vector2f edges(child_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding), child_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding));
            child_box.SetContent(Math::Max(Vector2f(0), Vector2f(rect.width, rect.height) - edges));
            const Vector2f offset(rect.x, rect.y);
            if (auto* text = rmlui_dynamic_cast<ElementText*>(child)) {
                TextSize(text, rect.width, true); child->SetBox(child_box);
                visible = Math::Max(visible, offset + Vector2f(rect.width, rect.height));
            } else {
                auto formatted = FormattingContext::FormatIndependent(container.get(), child, &child_box, FormattingContextType::Block);
                visible = Math::Max(visible, offset + formatted->GetVisibleOverflowSize());
                if (i == 0) formatted->GetBaselineOfLastLine(baseline);
            }
            child->SetOffset(box.GetPosition() + offset, element);
            scrollable = Math::Max(scrollable, offset + Vector2f(rect.width, rect.height) + Vector2f(child_box.GetEdge(BoxArea::Margin, BoxEdge::Right), child_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom)));
        }
        if (initial_size.y < 0 && !container->CatchOverflow(visible, scrollable, box, max_height)) continue;
        Box result = box;
        result.SetContent({measure_mode < 0 ? result_width : initial_size.x, initial_size.y < 0 ? result_height + scrollbar.y : initial_size.y});
        if (container->Close(visible, scrollable, result, result.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border) - baseline)) break;
    }
    return container;
}
}
