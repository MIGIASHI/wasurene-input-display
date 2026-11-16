#include "../src/input.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
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
	if (argc > 1 && std::string(argv[1]) == "--high-rate-test") {
#ifdef _WIN32
		wasurene_input::InputCapture input; input.start();
		bool ready = false;
		for (int i = 0; i < 200; ++i) {
			if (wasurene_input::read_mouse_motion()["available"].get<bool>()) {ready = true; break;}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		check(ready, "high-rate raw input did not start");
		auto before = wasurene_input::read_mouse_motion();
		INPUT event{}; event.type = INPUT_MOUSE;
		event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
		int64_t expected_x = 0, expected_y = 0;
		const auto started = std::chrono::steady_clock::now();
		for (int i = 0; i < 4000; ++i) {
			const auto due = started + std::chrono::microseconds(i * 250);
			while (std::chrono::steady_clock::now() < due) std::this_thread::yield();
			event.mi.dx = i % 7 + 1; event.mi.dy = -(i % 5 + 1);
			check(SendInput(1, &event, sizeof(event)) == 1, "paced 4000 Hz injection failed");
			expected_x += event.mi.dx; expected_y += event.mi.dy;
			if (i % 64 == 0) wasurene_input::read_mouse_motion();
		}
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		event.mi.dx = -3; event.mi.dy = 2;
		std::vector<INPUT> burst(4096, event);
		check(SendInput(UINT(burst.size()), burst.data(), sizeof(event)) == burst.size(), "backlog injection failed");
		expected_x += -3 * int64_t(burst.size()); expected_y += 2 * int64_t(burst.size());
		bool complete = false;
		for (int i = 0; i < 400; ++i) {
			auto motion = wasurene_input::read_mouse_motion();
			if (motion["x"].get<int64_t>() - before["x"].get<int64_t>() == expected_x &&
			    motion["y"].get<int64_t>() - before["y"].get<int64_t>() == expected_y) {complete = true; break;}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		check(complete, "high-rate or buffered motion was lost or counted twice");
		std::atomic<bool> sending{true};
		std::thread producer([&] {
			while (sending) {SendInput(64, burst.data(), sizeof(event)); std::this_thread::sleep_for(std::chrono::milliseconds(1));}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		const auto stopping = std::chrono::steady_clock::now();
		input.stop();
		sending = false; producer.join();
		check(std::chrono::steady_clock::now() - stopping < std::chrono::milliseconds(500), "high-rate input blocked shutdown");
		check(!wasurene_input::read_mouse_motion()["available"].get<bool>(), "motion survived high-rate shutdown");
		std::cout << "4000 paced reports (" << 4000 / seconds << " reports/s), 4096-report backlog: exact totals and busy shutdown OK" << std::endl;
#endif
		return 0;
	}
	if (argc > 1 && std::string(argv[1]) == "--motion-test") {
#ifdef _WIN32
		auto mouse_owner = [] {
			UINT count=0; GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE));
			std::vector<RAWINPUTDEVICE> devices(count);
			if(count)GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE));
			for(const auto &device:devices)if(device.usUsagePage==1 && device.usUsage==2)return device.hwndTarget;
			return HWND(nullptr);
		};
		HWND other=CreateWindowExW(0,L"STATIC",L"Other OBS input owner",WS_POPUP,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
		RAWINPUTDEVICE device{1,2,RIDEV_INPUTSINK,other};
		check(other && RegisterRawInputDevices(&device,1,sizeof(device)),"cannot register test input owner");
		wasurene_input::InputCapture input; input.start();
		for(int i=0;i<30;++i){check(!wasurene_input::read_mouse_motion()["available"].get<bool>(),"overrode another raw input owner");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
		input.stop();check(mouse_owner()==other,"removed another raw input owner on shutdown");
		device.dwFlags=RIDEV_REMOVE;device.hwndTarget=nullptr;RegisterRawInputDevices(&device,1,sizeof(device));DestroyWindow(other);
		input.start();bool ready=false;
		for(int i=0;i<200;++i){if(wasurene_input::read_mouse_motion()["available"].get<bool>()){ready=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
		check(ready,"raw mouse input did not start");
		auto before=wasurene_input::read_mouse_motion();POINT cursor;GetCursorPos(&cursor);
		INPUT event{};event.type=INPUT_MOUSE;event.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_MOVE_NOCOALESCE;event.mi.dx=24;event.mi.dy=-12;
		check(SendInput(1,&event,sizeof(event))==1,"CI mouse motion injection failed");
		bool seen=false;
		for(int i=0;i<100;++i){auto motion=wasurene_input::read_mouse_motion();if(motion["x"].get<int64_t>()>before["x"].get<int64_t>() && motion["y"].get<int64_t>()<before["y"].get<int64_t>()){seen=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
		SetCursorPos(cursor.x,cursor.y);check(seen,"relative mouse direction missing");
		auto first=wasurene_input::read_mouse_motion(),second=wasurene_input::read_mouse_motion();
		check(first["x"]==second["x"] && first["y"]==second["y"],"reading consumed mouse movement");
		std::this_thread::sleep_for(std::chrono::milliseconds(2200));check(!mouse_owner(),"idle raw input registration not released");
		input.stop();check(!mouse_owner(),"raw input registration survived shutdown");
		std::cout<<"Raw mouse direction, shared readers, registration conflict and cleanup OK"<<std::endl;
#endif
		return 0;
	}
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
