#include "../src/input.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

void check(bool ok, const char *message)
{
	if (!ok) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
	using wasurene_input::standard_pad;
	using wasurene_input::hat_direction;
	using json = nlohmann::json;
	if (argc > 1 && std::string(argv[1]) == "--wheel-test") {
#ifdef _WIN32
		wasurene_input::InputCapture input;
		input.start();
		for (int i = 0; i < 100; ++i) {
			wasurene_input::read_keys({256});
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		INPUT key{};
		key.type = INPUT_KEYBOARD; key.ki.wVk = 90;
		check(SendInput(1, &key, sizeof(key)) == 1, "keyboard input injection failed on CI desktop");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		bool key_seen = wasurene_input::read_keys({90})["pressed"] == json({90});
		key.ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(1, &key, sizeof(key));
		check(key_seen, "selected background keyboard state missing");
		INPUT event{};
		event.type = INPUT_MOUSE;
		event.mi.dwFlags = MOUSEEVENTF_WHEEL;
		event.mi.mouseData = WHEEL_DELTA;
		check(SendInput(1, &event, sizeof(event)) == 1, "wheel input injection failed on CI desktop");
		bool seen = false;
		for (int i = 0; i < 20; ++i) {
			auto pressed = wasurene_input::read_keys({256})["pressed"];
			if (pressed == json({256})) { seen = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		check(seen, "background mouse wheel was not captured");
		check(wasurene_input::read_keys({65})["pressed"].empty(), "unselected wheel leaked into key state");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		check(wasurene_input::read_keys({256})["pressed"].empty(), "wheel pulse did not expire");
		input.stop();
		std::cout << "Background wheel hook, selected-code filtering and pulse expiry OK" << std::endl;
#endif
		return 0;
	}
	if (argc > 1) {
		wasurene_input::InputCapture input;
		input.start();
		std::this_thread::sleep_for(std::chrono::milliseconds(750));
		std::cout << input.snapshot().dump() << std::endl;
		return 0;
	}
	const uint16_t masks[16] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0, 0,
				   0x20, 0x10, 0x40, 0x80, 1, 2, 4, 8};
	for (int b = 0; b < 16; ++b) {
		auto pad = standard_pad(2, masks[b], b == 6 ? 255 : 0, b == 7 ? 255 : 0, 0, 0, 0, 0);
		check(pad["index"] == 2 && pad["mapping"] == "standard", "identity");
		for (int i = 0; i < 17; ++i)
			check(pad["buttons"][i]["pressed"] == (i == b), "button mapping");
	}
	auto pad = standard_pad(0, 0, 127, 128, -32768, 32767, 32767, -32768);
	check(pad["axes"] == json({-1.0, -1.0, 1.0, 1.0}), "axis signs and bounds");
	check(pad["buttons"][6]["pressed"] == false && pad["buttons"][7]["pressed"] == true, "trigger threshold");
	check(hat_direction(0xffffffff) == json({0, 0}) && hat_direction(0xffff) == json({0, 0}), "POV neutral");
	const int directions[8][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}};
	for (int i = 0; i < 8; ++i)
		check(hat_direction(i * 4500) == json({directions[i][0], directions[i][1]}), "POV direction");
	wasurene_input::InputCapture input;
	for (int i = 0; i < 3; ++i) {
		input.start(); input.start();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		check(input.snapshot()["pads"].is_array(), "snapshot");
		input.stop(); input.stop();
		check(input.snapshot()["pads"].empty(), "stop clears input");
	}
	std::cout << "Controller mapping and lifecycle OK" << std::endl;
}
