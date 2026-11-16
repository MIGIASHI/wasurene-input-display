// Derived from MIGIASHI/sf6-tracker. SPDX-License-Identifier: GPL-2.0-or-later
// A very small HTTP/1.1 server for the overlay, the control panel and the JSON API.
// Listens on 127.0.0.1 only and answers only requests addressed to this PC.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace wasurene_input {

struct HttpRequest {
	std::string method, path, query, body;
	std::map<std::string, std::string> headers; // lower-case names
	std::string param(const std::string &name) const;
};

struct HttpResponse {
	int code = 200;
	std::string type = "application/json; charset=utf-8";
	std::string body;
	std::map<std::string, std::string> headers;
};

class HttpServer {
public:
	using Handler = std::function<HttpResponse(const HttpRequest &)>;
	explicit HttpServer(Handler handler);
	~HttpServer();
	bool start(int port, std::string &error);
	void stop();

private:
	void accept_loop();
	void serve(intptr_t sock);
	int receive(intptr_t sock, char *buffer, int size);

	Handler handler_;
	intptr_t listen_ = -1;
	std::atomic<bool> running_{false};
	std::mutex clients_mutex_;
	std::condition_variable clients_done_;
	std::set<intptr_t> clients_;
	std::thread thread_;
};

std::string url_decode(const std::string &s);

} // namespace wasurene_input
