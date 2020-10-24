#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "network.hpp"
#include "game_console.hpp"

#include "utils/hook.hpp"
#include "utils/string.hpp"

namespace network
{
	namespace
	{
		std::unordered_map<std::string, callback>& get_callbacks()
		{
			static std::unordered_map<std::string, callback> callbacks{};
			return callbacks;
		}

		bool handle_command(game::netadr_s* address, const char* command, game::msg_t* message)
		{
			const auto cmd_string = utils::string::to_lower(command);
			auto& callbacks = get_callbacks();
			const auto handler = callbacks.find(cmd_string);
			if (handler == callbacks.end())
			{
				return false;
			}

			const auto offset = cmd_string.size() + 5;
			const std::string_view data(message->data + offset, message->cursize - offset);

			handler->second(*address, data);
			return true;
		}

		void handle_command_stub(utils::hook::assembler& a)
		{
			const auto return_unhandled = a.newLabel();

			a.pushad64();

			a.mov(r8, rsi); // message
			a.mov(rdx, rdi); // command
			a.mov(rcx, r14); // netaddr

			a.call(handle_command);

			a.test(al, al);
			a.jz(return_unhandled);

			// Command handled
			a.popad64();
			a.jmp(0x1402C6DE8);

			a.bind(return_unhandled);
			a.popad64();
			a.jmp(0x1402C64EE);
		}

		int net_compare_base_address(const game::netadr_s* a1, const game::netadr_s* a2)
		{
			if (a1->type == a2->type)
			{
				switch (a1->type)
				{
				case game::netadrtype_t::NA_BOT:
				case game::netadrtype_t::NA_LOOPBACK:
					return a1->port == a2->port;

				case game::netadrtype_t::NA_IP:
					return !memcmp(a1->ip, a2->ip, 4);
				case game::netadrtype_t::NA_BROADCAST:
					return true;
				default:
					break;
				}
			}

			return false;
		}

		int net_compare_address(const game::netadr_s* a1, const game::netadr_s* a2)
		{
			return net_compare_base_address(a1, a2) && a1->port == a2->port;
		}
	}

	void on(const std::string& command, const callback& callback)
	{
		get_callbacks()[utils::string::to_lower(command)] = callback;
	}

	void send(const game::netadr_s& address, const std::string& command, const std::string& data, const char separator)
	{
		std::string packet = "\xFF\xFF\xFF\xFF";
		packet.append(command);
		packet.push_back(separator);
		packet.append(data);

		send(address, packet);
	}

	void send(const game::netadr_s& address, const std::string& data)
	{
		game::Sys_SendPacket(static_cast<int>(data.size()), data.data(), &address);
	}

	bool are_addresses_equal(const game::netadr_s& a, const game::netadr_s& b)
	{
		return net_compare_address(&a, &b);
	}

	const char* net_adr_to_string(const game::netadr_s& a)
	{
		if (a.type == game::netadrtype_t::NA_LOOPBACK)
		{
			return "loopback";
		}

		if (a.type == game::netadrtype_t::NA_BOT)
		{
			return "bot";
		}

		if (a.type == game::netadrtype_t::NA_IP || a.type == game::netadrtype_t::NA_BROADCAST)
		{
			if (a.port)
			{
				return utils::string::va("%u.%u.%u.%u:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3], htons(a.port));
			}

			return utils::string::va("%u.%u.%u.%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
		}

		return "bad";
	}

	void set_xuid_config_string_stub(utils::hook::assembler& a)
	{
		const auto return_regular = a.newLabel();

		a.mov(rax, ptr(rsp));
		a.mov(r9, 0x140477715); // This is the evil one :(
		a.cmp(rax, r9);
		a.jne(return_regular);

		// Do the original work
		a.call(return_regular);

		// Jump to success branch
		a.mov(rax, 0x14047771E);
		a.mov(ptr(rsp), rax);

		a.ret();

		a.bind(return_regular);

		a.sub(rsp, 0x38);
		a.mov(eax, ptr(rcx));
		a.mov(r9d, ptr(rcx, 4));
		a.mov(r10, rdx);

		a.jmp(0x14041DFBD);
	}

	class module final : public module_interface
	{
	public:
		void post_unpack() override
		{
			{
				if (game::environment::is_sp())
				{
					return;
				}

				// redirect dw_sendto to raw socket
				utils::hook::jump(0x140501AAA, reinterpret_cast<void*>(0x140501A3A));

				// intercept command handling
				utils::hook::jump(0x1402C64CB, utils::hook::assemble(handle_command_stub), true);

				// handle xuid without secure connection
				utils::hook::jump(0x14041DFB0, utils::hook::assemble(set_xuid_config_string_stub), true);

				utils::hook::jump(0x14041D010, net_compare_address);
				utils::hook::jump(0x14041D060, net_compare_base_address);

				// don't establish secure conenction
				utils::hook::set<uint8_t>(0x1402ECF1D, 0xEB);
				utils::hook::set<uint8_t>(0x1402ED02A, 0xEB);
				utils::hook::set<uint8_t>(0x1402ED34D, 0xEB);
				utils::hook::set<uint8_t>(0x1402C4A1F, 0xEB); //

				// ignore unregistered connection
				utils::hook::jump(0x140471AAC, reinterpret_cast<void*>(0x140471A50));
				utils::hook::set<uint8_t>(0x140471AF0, 0xEB);

				// disable xuid verification
				utils::hook::set<uint8_t>(0x140154AA9, 0xEB);
				utils::hook::set<uint8_t>(0x140154AC3, 0xEB);

				// disable xuid verification
				utils::hook::nop(0x140474584, 2);
				utils::hook::set<uint8_t>(0x1404745DB, 0xEB);

				// ignore configstring mismatch
				utils::hook::set<uint8_t>(0x1402CCCC7, 0xEB);

				// ignore dw handle in SV_PacketEvent
				utils::hook::set<uint8_t>(0x14047A17A, 0xEB);
				utils::hook::call(0x14047A16E, &net_compare_address);

				// ignore dw handle in SV_FindClientByAddress
				utils::hook::set<uint8_t>(0x1404799AD, 0xEB);
				utils::hook::call(0x1404799A1, &net_compare_address);

				// ignore dw handle in SV_DirectConnect
				utils::hook::set<uint8_t>(0x1404717BA, 0xEB);
				utils::hook::set<uint8_t>(0x1404719DF, 0xEB);
				utils::hook::call(0x1404717AD, &net_compare_address);
				utils::hook::call(0x1404719D2, &net_compare_address);

				// increase cl_maxpackets
				utils::hook::set<uint8_t>(0x1402C8083, 125);
				utils::hook::set<uint8_t>(0x1402C808C, 125);

				// ignore impure client
				utils::hook::jump(0x140472671, reinterpret_cast<void*>(0x1404726F9));

				// don't send checksum
				utils::hook::set<uint8_t>(0x140501A63, 0);

				// don't read checksum
				utils::hook::jump(0x1405019CB, 0x1405019F3);

				// ignore built in "print" oob command and add in our own
				utils::hook::set<uint8_t>(0x1402C6AA4, 0xEB);
				on("print", [](const game::netadr_s& addr, const std::string_view& data)
				{
					const std::string message{ data };

					if (game::environment::is_dedi())
					{
						printf("%s\n", message.data());
					}
					else
					{
						game_console::print(7, "%s\n", message.data());
					}
				});
			}
		}
	};
}

REGISTER_MODULE(network::module)
