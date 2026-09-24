#include "ElementBackgroundBorder.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "BoxShadowCache.h"
#include "GeometryBoxShadow.h"

namespace Rml {

ElementBackgroundBorder::ElementBackgroundBorder() {}

void ElementBackgroundBorder::Render(Element* element)
{
	if (background_dirty || border_dirty)
	{
		for (auto& background : backgrounds)
		{
			if (background.first != BackgroundType::Background && background.first != BackgroundType::Border)
				background.second.geometry.Release();
		}

		GenerateGeometry(element);

		background_dirty = false;
		border_dirty = false;
	}

	if (Background* shadow = GetBackground(BackgroundType::BoxShadowAndBackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		element->GetRenderManager()->GetRenderInterface()->SetPaintRole(PaintRole::Unknown);
		shadow->box_shadow_and_background_border->geometry.Render(offset, shadow->box_shadow_and_background_border->texture);
	}
	else
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		RenderInterface* render_interface = element->GetRenderManager()->GetRenderInterface();
		if (Background* background = GetBackground(BackgroundType::Background))
		{
			render_interface->SetPaintRole(PaintRole::Background);
			background->geometry.Render(offset);
		}
		if (Background* border = GetBackground(BackgroundType::Border))
		{
			render_interface->SetPaintRole(PaintRole::Border);
			border->geometry.Render(offset);
		}
		render_interface->SetPaintRole(PaintRole::Unknown);
	}
}

void ElementBackgroundBorder::DirtyBackground()
{
	background_dirty = true;
}

void ElementBackgroundBorder::DirtyBorder()
{
	border_dirty = true;
}

Geometry* ElementBackgroundBorder::GetClipGeometry(Element* element, BoxArea clip_area)
{
	BackgroundType type = {};
	switch (clip_area)
	{
	case Rml::BoxArea::Border: type = BackgroundType::ClipBorder; break;
	case Rml::BoxArea::Padding: type = BackgroundType::ClipPadding; break;
	case Rml::BoxArea::Content: type = BackgroundType::ClipContent; break;
	default: RMLUI_ERROR; return nullptr;
	}

	RenderManager* render_manager = element->GetRenderManager();
	Geometry& geometry = GetOrCreateBackground(type).geometry;
	if (render_manager && !geometry)
	{
		Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
		MeshUtilities::GenerateBackground(mesh, element->GetRenderBox(clip_area), ColourbPremultiplied(255));
		geometry = render_manager->MakeGeometry(std::move(mesh));
	}

	return &geometry;
}

ElementBackgroundBorder::Background* ElementBackgroundBorder::GetBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return &it->second;
	return nullptr;
}

ElementBackgroundBorder::Background& ElementBackgroundBorder::GetOrCreateBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return it->second;

	Background& background = backgrounds[type];
	return background;
}

void ElementBackgroundBorder::EraseBackground(BackgroundType type)
{
	backgrounds.erase(type);
}

void ElementBackgroundBorder::GenerateGeometry(Element* element)
{
	RMLUI_ZoneScoped;
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	const ComputedValues& computed = element->GetComputedValues();
	const bool has_box_shadow = computed.has_box_shadow();

	if (has_box_shadow)
	{
		// The box shadow geometry also includes the element's background and border, thus we can skip the normal background generation.
		EraseBackground(BackgroundType::Background);
		EraseBackground(BackgroundType::Border);
		Background& shadow_background = GetOrCreateBackground(BackgroundType::BoxShadowAndBackgroundBorder);
		shadow_background.box_shadow_and_background_border = BoxShadowCache::GetHandle(element, computed);
		return;
	}

	EraseBackground(BackgroundType::BoxShadowAndBackgroundBorder);

	const float opacity = computed.opacity();
	ColourbPremultiplied background_color = computed.background_color().ToPremultiplied(opacity);
	Array<ColourbPremultiplied, 4> border_colors = {
		computed.border_top_color().ToPremultiplied(opacity),
		computed.border_right_color().ToPremultiplied(opacity),
		computed.border_bottom_color().ToPremultiplied(opacity),
		computed.border_left_color().ToPremultiplied(opacity),
	};

	Geometry& background_geometry = GetOrCreateBackground(BackgroundType::Background).geometry;
	Geometry& border_geometry = GetOrCreateBackground(BackgroundType::Border).geometry;
	Mesh background_mesh = background_geometry.Release(Geometry::ReleaseMode::ClearMesh);
	Mesh border_mesh = border_geometry.Release(Geometry::ReleaseMode::ClearMesh);
	const ColourbPremultiplied transparent(0, 0, 0, 0);

	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox render_box = element->GetRenderBox(BoxArea::Padding, i);
		MeshUtilities::GenerateBackground(background_mesh, render_box, background_color);
		MeshUtilities::GenerateBackgroundBorder(border_mesh, render_box, transparent, border_colors.data());
	}

	background_geometry = render_manager->MakeGeometry(std::move(background_mesh));
	border_geometry = render_manager->MakeGeometry(std::move(border_mesh));
}

} // namespace Rml
