#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "localized_strings.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"

namespace branding
{
	class module final : public module_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			if (game::environment::is_mp())
			{
				localized_strings::override("LUA_MENU_MULTIPLAYER_CAPS", "IW6x: MULTIPLAYER\n");
			}

			localized_strings::override("LUA_MENU_LEGAL_COPYRIGHT", "IW6x: Pre-Release by X Labs.\n");

			scheduler::loop([]()
			{
				const auto x = 3;
				const auto y = 0;
				const auto scale = 0.5f;
				float color[4] = {1.0f, 1.0f, 1.0f, 0.5f};
				const auto* text = "IW6x: Pre-Release";

				auto* font = game::R_RegisterFont("fonts/normalfont");
				if (!font) return;

				game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x, y + static_cast<float>(font->pixelHeight) * scale,
				                       scale, scale, 0.0, color, 0);
			}, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_MODULE(branding::module)
