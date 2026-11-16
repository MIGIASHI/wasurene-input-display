// Derived from MIGIASHI/sf6-tracker. SPDX-License-Identifier: GPL-2.0-or-later
#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CLOSESOCK close
#endif

namespace wasurene_input {

namespace {

const char *reason(int code)
{
	switch (code) {
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 413:
		return "Payload Too Large";
	default:
		return "Error";
	}
}

bool send_all(intptr_t s, const std::string &data)
{
	size_t off = 0;
	while (off < data.size()) {
		int flags = 0;
#ifdef MSG_NOSIGNAL
		flags = MSG_NOSIGNAL;
#endif
		int n = send((decltype(socket(0, 0, 0)))s, data.data() + off, int(std::min<size_t>(data.size() - off, 1 << 20)), flags);
		if (n <= 0)
			return false;
		off += size_t(n);
	}
	return true;
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

} // namespace

std::string url_decode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out += ' ';
		} else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
			   std::isxdigit((unsigned char)s[i + 2])) {
			out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string HttpRequest::param(const std::string &name) const
{
	size_t start = 0;
	while (start <= query.size()) {
		size_t end = query.find('&', start);
		if (end == std::string::npos)
			end = query.size();
		std::string kv = query.substr(start, end - start);
		size_t eq = kv.find('=');
		if (url_decode(kv.substr(0, eq)) == name)
			return eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
		start = end + 1;
	}
	return "";
}

HttpServer::HttpServer(Handler handler) : handler_(std::move(handler)) {}

HttpServer::~HttpServer()
{
	stop();
}

bool HttpServer::start(int port, std::string &error)
{
	if (running_)
		return true;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		error = "cannot initialize sockets";
		return false;
	}
#endif
	auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
	if (s == INVALID_SOCKET) {
#else
	if (s < 0) {
#endif
		error = "cannot create a socket";
#ifdef _WIN32
		WSACleanup();
#endif
		return false;
	}
#ifdef _WIN32
	BOOL excl = TRUE; // never share the port with another program
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&excl), sizeof excl);
#else
	int yes = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(uint16_t(port));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this PC only
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(s, 16) != 0) {
		CLOSESOCK(s);
#ifdef _WIN32
		WSACleanup();
#endif
		error = "port " + std::to_string(port) + " is in use (is the Python tracker running?)";
		return false;
	}
	listen_ = intptr_t(s);
	running_ = true;
	thread_ = std::thread(&HttpServer::accept_loop, this);
	return true;
}

void HttpServer::stop()
{
	if (!running_.exchange(false))
		return;
#ifdef _WIN32
	closesocket((SOCKET)listen_);
#else
	shutdown(int(listen_), SHUT_RDWR);
	close(int(listen_));
#endif
	if (thread_.joinable())
		thread_.join();
	// Wake blocked reads/writes, then wait until no client can touch this server or its handler.
	std::unique_lock<std::mutex> lock(clients_mutex_);
	for (intptr_t client : clients_) {
#ifdef _WIN32
		shutdown((SOCKET)client, SD_BOTH);
#else
		shutdown(int(client), SHUT_RDWR);
#endif
	}
	clients_done_.wait(lock, [this] { return clients_.empty(); });
#ifdef _WIN32
	WSACleanup();
#endif
}

void HttpServer::accept_loop()
{
	while (running_) {
		sockaddr_in peer{};
		socklen_t len = sizeof peer;
		auto c = accept((decltype(socket(0, 0, 0)))listen_, reinterpret_cast<sockaddr *>(&peer), &len);
#ifdef _WIN32
		if (c == INVALID_SOCKET) {
#else
		if (c < 0) {
#endif
			if (!running_)
				break;
			continue;
		}
		std::lock_guard<std::mutex> lock(clients_mutex_);
		clients_.insert(intptr_t(c));
		try {
			std::thread([this, c] {
				try {
					serve(intptr_t(c));
				} catch (...) {
					// Always release the connection, even if handling the request fails.
				}
				std::lock_guard<std::mutex> lock(clients_mutex_);
				CLOSESOCK(c);
				clients_.erase(intptr_t(c));
				clients_done_.notify_all();
			}).detach();
		} catch (...) {
			CLOSESOCK(c);
			clients_.erase(intptr_t(c));
		}
	}
}

int HttpServer::receive(intptr_t sock, char *buffer, int size)
{
	auto s = (decltype(socket(0, 0, 0)))sock;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (running_ && std::chrono::steady_clock::now() < deadline) {
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(s, &readable);
		timeval wait{0, 100000};
#ifdef _WIN32
		int ready = select(0, &readable, nullptr, nullptr, &wait);
#else
		int ready = select(int(s) + 1, &readable, nullptr, nullptr, &wait);
#endif
		if (ready < 0 || !running_)
			return -1;
		if (ready > 0)
			return recv(s, buffer, size, 0);
	}
	return -1;
}

void HttpServer::serve(intptr_t sock)
{
	auto s = (decltype(socket(0, 0, 0)))sock;
#ifdef _WIN32
	DWORD tv = 5000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tv), sizeof tv);
#else
	timeval tv{5, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
	std::string buf;
	char chunk[8192];
	size_t head_end = std::string::npos;
	while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
		int n = receive(sock, chunk, sizeof chunk);
		if (n <= 0 || buf.size() > 65536) {
			return;
		}
		buf.append(chunk, size_t(n));
	}
	HttpRequest req;
	std::string head = buf.substr(0, head_end);
	size_t line_end = head.find("\r\n");
	std::string first = head.substr(0, line_end);
	size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) {
		return;
	}
	req.method = first.substr(0, sp1);
	std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t q = target.find('?');
	req.path = url_decode(target.substr(0, q));
	req.query = q == std::string::npos ? "" : target.substr(q + 1);
	size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
	while (pos < head.size()) {
		size_t e = head.find("\r\n", pos);
		if (e == std::string::npos)
			e = head.size();
		std::string line = head.substr(pos, e - pos);
		size_t colon = line.find(':');
		if (colon != std::string::npos)
			req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		pos = e + 2;
	}
	size_t want = 0;
	auto cl = req.headers.find("content-length");
	if (cl != req.headers.end())
		want = size_t(std::strtoull(cl->second.c_str(), nullptr, 10));
	HttpResponse res;
	if (want > (16u << 20)) {
		res.code = 413;
		res.body = "{\"error\":\"too large\"}";
	} else {
		req.body = buf.substr(head_end + 4);
		while (req.body.size() < want) {
			int n = receive(sock, chunk, sizeof chunk);
			if (n <= 0)
				break;
			req.body.append(chunk, size_t(n));
		}
		req.body.resize(std::min(req.body.size(), want));
		if (!running_ || req.body.size() != want)
			return;
		try {
			res = handler_(req);
		} catch (const std::exception &e) {
			res.code = 500;
			res.body = std::string("{\"error\":\"") + e.what() + "\"}";
		}
	}
	std::string out = "HTTP/1.1 " + std::to_string(res.code) + " " + reason(res.code) + "\r\n";
	out += "Content-Type: " + res.type + "\r\n";
	out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
	out += "Cache-Control: no-store\r\nConnection: close\r\n";
	for (const auto &h : res.headers)
		out += h.first + ": " + h.second + "\r\n";
	out += "\r\n";
	send_all(sock, out) && send_all(sock, res.body);
}

} // namespace wasurene_input
