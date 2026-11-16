// Derived from MIGIASHI/sf6-tracker. SPDX-License-Identifier: GPL-2.0-or-later
#include "input.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <map>
#include <set>
#include <vector>
#endif

namespace wasurene_input {
using json = nlohmann::json;

#ifdef _WIN32
namespace {
std::atomic<ULONGLONG> wheel_until[4]{};
std::atomic<ULONGLONG> wheel_requested_until{0};
LRESULT CALLBACK wheel_hook(int code, WPARAM message, LPARAM data)
{
	if (code >= 0 && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
		auto *event = reinterpret_cast<MSLLHOOKSTRUCT *>(data);
		short delta = short(event->mouseData >> 16);
		int direction = message == WM_MOUSEWHEEL ? (delta > 0 ? 0 : 1) : (delta > 0 ? 3 : 2);
		if (delta) wheel_until[direction] = GetTickCount64() + 120;
	}
	return CallNextHookEx(nullptr, code, message, data);
}
}
#endif

json read_keys(const json &codes)
{
	json pressed = json::array();
#ifdef _WIN32
	if (codes.is_array() && codes.size() <= 32)
		for (const auto &code : codes) {
			if (!code.is_number_integer()) continue;
			int key = code.get<int>();
			if (key >= 256 && key <= 259) {
				wheel_requested_until = GetTickCount64() + 2000;
				if (GetTickCount64() < wheel_until[key-256].load()) pressed.push_back(key);
			} else if (key > 0 && key < 255 && (GetAsyncKeyState(key) & 0x8000)) pressed.push_back(key);
		}
	return {{"supported", true}, {"pressed", pressed}};
#else
	(void)codes;
	return {{"supported", false}, {"pressed", pressed}};
#endif
}

json hat_direction(uint32_t pov)
{
	if ((pov & 0xffff) == 0xffff)
		return {0, 0};
	static const int dirs[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
	const auto &d = dirs[((pov + 2250) / 4500) % 8];
	return {d[0], d[1]};
}

json standard_pad(int index, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
	// A B X Y LB RB LT RT Back Start L3 R3 Up Down Left Right Guide.
	static const uint16_t masks[17] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0, 0,
					 0x20, 0x10, 0x40, 0x80, 1, 2, 4, 8, 0};
	json values = json::array();
	for (int i = 0; i < 17; ++i) {
		double v = i == 6 ? lt / 255.0 : i == 7 ? rt / 255.0 : (buttons & masks[i]) ? 1.0 : 0.0;
		values.push_back({{"pressed", v > 0.5}, {"value", v}});
	}
	auto axis = [](int value) { return std::clamp(value / 32767.0, -1.0, 1.0); };
	return {{"index", index}, {"id", "XInput " + std::to_string(index + 1)}, {"mapping", "standard"},
		{"buttons", values}, {"axes", {axis(lx), axis(-int(ly)), axis(rx), axis(-int(ry))}}};
}

void InputCapture::start()
{
	if (!running_.exchange(true))
		thread_ = std::thread(&InputCapture::loop, this);
}

void InputCapture::stop()
{
	if (!running_.exchange(false))
		return;
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();
	std::lock_guard<std::mutex> lock(mutex_);
	pads_ = json::array();
}

json InputCapture::snapshot()
{
	std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
	return {{"supported", true}, {"pads", pads_}};
#else
	return {{"supported", false}, {"pads", pads_}};
#endif
}

#ifdef _WIN32
namespace {
struct Joysticks {
	IDirectInput8W *di = nullptr;
	HWND window = nullptr;
	struct Device { IDirectInputDevice8W *ptr; int index; std::string name; };
	std::map<std::wstring, Device> devices;
	std::map<std::wstring, int> indices;
	std::set<std::wstring> seen;
	static BOOL CALLBACK set_range(const DIDEVICEOBJECTINSTANCEW *object, void *context)
	{
		auto *device = static_cast<IDirectInputDevice8W *>(context);
		DIPROPRANGE range{{sizeof(DIPROPRANGE), sizeof(DIPROPHEADER), object->dwType, DIPH_BYID}, -32767, 32767};
		device->SetProperty(DIPROP_RANGE, &range.diph);
		return DIENUM_CONTINUE;
	}

	static BOOL CALLBACK enumerate(const DIDEVICEINSTANCEW *info, void *context)
	{
		auto &self = *static_cast<Joysticks *>(context);
		wchar_t key[40];
		StringFromGUID2(info->guidInstance, key, 40);
		self.seen.insert(key);
		if (self.devices.count(key))
			return DIENUM_CONTINUE;
		IDirectInputDevice8W *device = nullptr;
		if (FAILED(self.di->CreateDevice(info->guidInstance, &device, nullptr)))
			return DIENUM_CONTINUE;
		DIPROPGUIDANDPATH path{};
		path.diph = {sizeof(path), sizeof(DIPROPHEADER), 0, DIPH_DEVICE};
		// XInput devices also appear as DirectInput: do not report the same buttons twice.
		if (SUCCEEDED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph)) && wcsstr(path.wszPath, L"IG_")) {
			device->Release();
			return DIENUM_CONTINUE;
		}
		if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) ||
		    FAILED(device->SetCooperativeLevel(self.window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
			device->Release();
			return DIENUM_CONTINUE;
		}
		device->EnumObjects(set_range, device, DIDFT_AXIS);
		device->Acquire();
		if (!self.indices.count(key))
			self.indices[key] = int(self.indices.size()) + 4;
		int size = WideCharToMultiByte(CP_UTF8, 0, info->tszInstanceName, -1, nullptr, 0, nullptr, nullptr);
		std::string name(std::max(1, size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, info->tszInstanceName, -1, name.data(), size, nullptr, nullptr);
		name.resize(std::max(0, size - 1));
		self.devices.emplace(key, Device{device, self.indices[key], "DirectInput: " + name});
		return DIENUM_CONTINUE;
	}

	~Joysticks()
	{
		for (auto &entry : devices) { entry.second.ptr->Unacquire(); entry.second.ptr->Release(); }
		if (di) di->Release();
		if (window) DestroyWindow(window);
	}
	void refresh()
	{
		if (!di || !window) return;
		seen.clear();
		di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate, this, DIEDFL_ATTACHEDONLY);
		for (auto it = devices.begin(); it != devices.end();) {
			if (!seen.count(it->first)) {
				it->second.ptr->Unacquire(); it->second.ptr->Release(); it = devices.erase(it);
			} else ++it;
		}
	}
	void read(json &pads)
	{
		for (auto &entry : devices) {
			auto &device = entry.second;
			DIJOYSTATE2 state{};
			if (FAILED(device.ptr->Poll())) { device.ptr->Acquire(); device.ptr->Poll(); }
			if (FAILED(device.ptr->GetDeviceState(sizeof(state), &state))) continue;
			json buttons = json::array();
			for (auto button : state.rgbButtons)
				buttons.push_back({{"pressed", (button & 0x80) != 0}, {"value", (button & 0x80) ? 1 : 0}});
			auto axis = [](LONG value) { return std::clamp(value / 32767.0, -1.0, 1.0); };
			pads.push_back({{"index", device.index}, {"id", device.name}, {"mapping", ""},
				       {"buttons", buttons}, {"axes", {axis(state.lX), axis(state.lY), axis(state.lZ), axis(state.lRz)}},
				       {"direction", hat_direction(state.rgdwPOV[0])}});
		}
	}
};
} // namespace
#endif

void InputCapture::loop()
{
#ifdef _WIN32
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		Joysticks sticks;
		HHOOK wheel = nullptr;
		sticks.window = CreateWindowExW(0, L"STATIC", L"SF6 Tracker input", WS_POPUP,
					      0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
				  reinterpret_cast<void **>(&sticks.di), nullptr);
		auto next_scan = std::chrono::steady_clock::time_point{};
		while (running_) {
			if (!wheel && GetTickCount64() < wheel_requested_until.load()) {
				HMODULE module = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   reinterpret_cast<LPCWSTR>(&wheel_hook), &module);
				wheel = SetWindowsHookExW(WH_MOUSE_LL, wheel_hook, module, 0);
			} else if (wheel && GetTickCount64() >= wheel_requested_until.load()) {
				UnhookWindowsHookEx(wheel); wheel = nullptr;
			}
			auto now = std::chrono::steady_clock::now();
			if (now >= next_scan) { sticks.refresh(); next_scan = now + std::chrono::seconds(2); }
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
			json pads = json::array();
			for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
				XINPUT_STATE state{};
				if (XInputGetState(i, &state) == ERROR_SUCCESS) {
					auto &g = state.Gamepad;
					pads.push_back(standard_pad(int(i), g.wButtons, g.bLeftTrigger, g.bRightTrigger,
							    g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY));
				}
			}
			sticks.read(pads);
			std::unique_lock<std::mutex> lock(mutex_);
			pads_ = std::move(pads);
			wake_.wait_for(lock, std::chrono::milliseconds(8), [this] { return !running_.load(); });
		}
		if (wheel) UnhookWindowsHookEx(wheel);
	}
	if (SUCCEEDED(com)) CoUninitialize();
#endif
}

} // namespace wasurene_input
