// Derived from MIGIASHI/sf6-tracker. SPDX-License-Identifier: GPL-2.0-or-later
// Controller input independent of Chromium's focused-page Gamepad API.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

namespace wasurene_input {

// Browser standard mapping, so existing input display URLs keep their assignments.
nlohmann::json standard_pad(int index, uint16_t buttons, uint8_t lt, uint8_t rt,
			   int16_t lx, int16_t ly, int16_t rx, int16_t ry);
nlohmann::json hat_direction(uint32_t pov);
nlohmann::json read_keys(const nlohmann::json &codes);
nlohmann::json read_mouse_motion();

class InputCapture {
public:
	~InputCapture() { stop(); }
	void start();
	void stop();
	nlohmann::json snapshot();

private:
	void loop();
	std::atomic<bool> running_{false};
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable wake_;
	nlohmann::json pads_ = nlohmann::json::array();
};

} // namespace wasurene_input
