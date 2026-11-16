// A real HTTP server controlled through stdin, without OBS or OCR dependencies.
#include "../src/http.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	std::atomic<int> handled{0};
	wasurene_input::HttpServer server([&](const wasurene_input::HttpRequest &req) {
		if (req.path == "/slow") {
			std::cout << "HANDLING" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(2400));
		}
		++handled;
		wasurene_input::HttpResponse response;
		response.body = req.path == "/large" ? std::string(16 << 20, 'x') : "{\"ok\":true}";
		return response;
	});
	std::string error;
	const int port = std::atoi(argv[1]);
	if (!server.start(port, error)) {
		std::cerr << error << std::endl;
		return 1;
	}
	std::cout << "READY" << std::endl;
	std::string command;
	while (std::getline(std::cin, command)) {
		server.stop();
		if (command != "restart")
			break;
		if (!server.start(port, error))
			return 1;
		std::cout << "READY" << std::endl;
	}
	server.stop(); // stopping twice is safe
	std::cout << "STOPPED " << handled.load() << std::endl;
}
