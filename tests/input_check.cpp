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
	if (argc > 1 && std::string(argv[1]) == "--keys-test") {
#ifdef _WIN32
		auto mouse_owner=[] {
			UINT count=0;GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE));
			std::vector<RAWINPUTDEVICE> devices(count);
			if(count)GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE));
			for(const auto &device:devices)if(device.usUsagePage==1 && device.usUsage==2)return device.hwndTarget;
			return HWND(nullptr);
		};
		wasurene_input::InputCapture input;input.start();
		for(int i=0;i<40;++i){check(wasurene_input::read_keys({256,257,258,259})["pressed"].empty(),"retired scroll code active");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
		check(!mouse_owner(),"plugin registered mouse input");input.stop();
		HWND other=CreateWindowExW(0,L"STATIC",L"Other input owner",WS_POPUP,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
		RAWINPUTDEVICE registration{1,2,RIDEV_INPUTSINK,other};
		check(other && RegisterRawInputDevices(&registration,1,sizeof(registration)),"test owner registration failed");
		input.start();
		INPUT keys[2]{};keys[0].type=INPUT_KEYBOARD;keys[0].ki.wVk=87;keys[1].type=INPUT_MOUSE;keys[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
		check(SendInput(2,keys,sizeof(INPUT))==2,"keyboard/click injection failed");
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const bool seen=wasurene_input::read_keys({87,1})["pressed"]==json({87,1});
		keys[0].ki.dwFlags=KEYEVENTF_KEYUP;keys[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,keys,sizeof(INPUT));
		check(seen,"selected keyboard/click state missing");input.stop();
		check(mouse_owner()==other,"plugin changed another mouse input registration");
		registration.dwFlags=RIDEV_REMOVE;registration.hwndTarget=nullptr;RegisterRawInputDevices(&registration,1,sizeof(registration));DestroyWindow(other);
		std::cout<<"Selected keys/clicks OK; removed scroll codes inert; no mouse registration; other owner preserved"<<std::endl;
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
