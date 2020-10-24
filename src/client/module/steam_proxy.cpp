#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "steam_proxy.hpp"
#include "scheduler.hpp"

#include "utils/nt.hpp"
#include "utils/string.hpp"
#include "utils/binary_resource.hpp"

#include "game/game.hpp"

#include "steam/interface.hpp"
#include "steam/steam.hpp"

namespace steam_proxy
{
	namespace
	{
		utils::binary_resource runner_file(RUNNER, "runner.exe");
	}

	class module final : public module_interface
	{
	public:
		void post_load() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			this->load_client();
			this->clean_up_on_error();

#ifndef DEV_BUILD
	try
	{
		this->start_mod("\xF0\x9F\x90\x8D" " IW6x: "s + (game::environment::is_sp() ? "Singleplayer" : "Multiplayer"),
		                game::environment::is_sp() ? 209160 : 209170);
	}
	catch (std::exception& e)
	{
		printf("Steam: %s\n", e.what());
	}
#endif
		}

		void pre_destroy() override
		{
			if (this->steam_client_module_)
			{
				if (this->steam_pipe_)
				{
					if (this->global_user_)
					{
						this->steam_client_module_.invoke<void>("Steam_ReleaseUser", this->steam_pipe_,
						                                        this->global_user_);
					}

					this->steam_client_module_.invoke<bool>("Steam_BReleaseSteamPipe", this->steam_pipe_);
				}
			}
		}

		const utils::nt::module& get_overlay_module() const
		{
			return steam_overlay_module_;
		}

	private:
		utils::nt::module steam_client_module_{};
		utils::nt::module steam_overlay_module_{};

		steam::interface client_engine_{};
		steam::interface client_user_{};
		steam::interface client_utils_{};

		void* steam_pipe_ = nullptr;
		void* global_user_ = nullptr;

		void* load_client_engine() const
		{
			if (!this->steam_client_module_) return nullptr;

			for (auto i = 1; i > 0; ++i)
			{
				std::string name = utils::string::va("CLIENTENGINE_INTERFACE_VERSION%03i", i);
				auto* const client_engine = this->steam_client_module_
				                                .invoke<void*>("CreateInterface", name.data(), nullptr);
				if (client_engine) return client_engine;
			}

			return nullptr;
		}

		void load_client()
		{
			const std::filesystem::path steam_path = steam::SteamAPI_GetSteamInstallPath();
			if (steam_path.empty()) return;

			utils::nt::module::load(steam_path / "tier0_s64.dll");
			utils::nt::module::load(steam_path / "vstdlib_s64.dll");
			this->steam_overlay_module_ = utils::nt::module::load(steam_path / "gameoverlayrenderer64.dll");
			this->steam_client_module_ = utils::nt::module::load(steam_path / "steamclient64.dll");
			if (!this->steam_client_module_) return;

			this->client_engine_ = load_client_engine();
			if (!this->client_engine_) return;

			this->steam_pipe_ = this->steam_client_module_.invoke<void*>("Steam_CreateSteamPipe");
			this->global_user_ = this->steam_client_module_.invoke<void*>(
				"Steam_ConnectToGlobalUser", this->steam_pipe_);
			this->client_user_ = this->client_engine_.invoke<void*>(8, this->steam_pipe_, this->global_user_);
			// GetIClientUser
			this->client_utils_ = this->client_engine_.invoke<void*>(13, this->steam_pipe_); // GetIClientUtils
		}

		void start_mod(const std::string& title, size_t app_id)
		{
			if (!this->client_utils_ || !this->client_user_) return;

			if (!this->client_user_.invoke<bool>("BIsSubscribedApp", app_id))
			{
				app_id = 480; // Spacewar
			}

			this->client_utils_.invoke<void>("SetAppIDForCurrentPipe", app_id, false);

			char our_directory[MAX_PATH] = {0};
			GetCurrentDirectoryA(sizeof(our_directory), our_directory);

			const auto path = runner_file.get_extracted_file();
			const std::string cmdline = utils::string::va("\"%s\" -proc %d", path.data(), GetCurrentProcessId());

			steam::game_id game_id;
			game_id.raw.type = 1; // k_EGameIDTypeGameMod
			game_id.raw.app_id = app_id & 0xFFFFFF;

			const auto* mod_id = "IW6x";
			game_id.raw.mod_id = *reinterpret_cast<const unsigned int*>(mod_id) | 0x80000000;

			this->client_user_.invoke<bool>("SpawnProcess", path.data(), cmdline.data(), our_directory,
			                                &game_id.bits, title.data(), 0, 0, 0);
		}

		void clean_up_on_error()
		{
			scheduler::schedule([this]()
			{
				if (this->steam_client_module_
					&& this->steam_pipe_
					&& this->global_user_
					&& this->steam_client_module_.invoke<bool>("Steam_BConnected", this->global_user_,
					                                           this->steam_pipe_)
					&& this->steam_client_module_.invoke<bool>("Steam_BLoggedOn", this->global_user_, this->steam_pipe_)
				)
				{
					return scheduler::cond_continue;
				}

				this->client_engine_ = nullptr;
				this->client_user_ = nullptr;
				this->client_utils_ = nullptr;

				this->steam_pipe_ = nullptr;
				this->global_user_ = nullptr;

				this->steam_client_module_ = utils::nt::module{nullptr};

				return scheduler::cond_end;
			});
		}
	};

	const utils::nt::module& get_overlay_module()
	{
		// TODO: Find a better way to do this
		return module_loader::get<module>()->get_overlay_module();
	}
}

REGISTER_MODULE(steam_proxy::module)
