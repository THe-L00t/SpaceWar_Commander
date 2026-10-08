#include "UI.h"

namespace swc {

	UI::UI() = default;
	UI::~UI() = default;

	UI::ElementId UI::AddSprite(const UISprite& sprite)
	{
		sprites.push_back(sprite);
		return ElementId(sprites.size() - 1);
	}

	void UI::Clear()
	{
		sprites.clear();
	}

	void UI::Extract(std::vector<UISprite>& out) const
	{
		out.insert(out.end(), sprites.begin(), sprites.end());
	}

} // namespace swc
