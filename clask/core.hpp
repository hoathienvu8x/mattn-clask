#include <iostream>
#include <functional>
#include <utility>
#include <vector>
#include <string>
#include <map>
#include <exception>
#include <sstream>
#include <iomanip>

#include <cstdio>
#include <ctime>

#ifdef _WIN32
# include <ws2tcpip.h>
static void
socket_perror(const char *s) {
  char buf[512];
  FormatMessage(
      FORMAT_MESSAGE_FROM_SYSTEM,
      nullptr,
      WSAGetLastError(),
      0,
      buf,
      sizeof(buf) / sizeof(buf[0]),
      nullptr);
  std::cerr << s << ": " << buf << std::endl;
}
#else
# include <sys/fcntl.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
#define closesocket(fd) close(fd)
#define socket_perror(s) perror(s)
#endif

#include "picohttpparser.h"
#include "picohttpparser.c"

namespace clask {

enum log_level {ERR, WARN, INFO, DEBUG};

class logger {
protected:
  std::ostringstream os;
  log_level lv;
  static log_level default_level;

private:
  logger(const logger&);
  logger& operator =(const logger&);

public:
  logger() {};
  virtual ~logger();
  std::ostringstream& get(log_level level = INFO);
  static log_level& level();
};

std::ostringstream& logger::get(log_level level) {
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  os << std::put_time(&tm, "%Y/%m/%d %H:%M:%S ");
  switch (level) {
    case ERR: os << "ERR: "; break;
    case WARN: os << "WARN: "; break;
    case INFO: os << "INFO: "; break;
    default: os << "DEBUG: "; break;
  }
  lv = level;
  return os;
}

logger::~logger() {
  if (lv >= logger::level()) {
    os << std::endl;
    std::cerr << os.str().c_str();
  }
}

typedef std::pair<std::string, std::string> header;

class response {
private:
  std::vector<header> headers;
  bool header_out;
  int s;
public:
  response(int s) : s(s), header_out(false) { }
  void set_header(std::string, std::string);
  void write(std::string);
};

void response::set_header(std::string key, std::string value) {
  headers.push_back(std::make_pair(key, value));
}

void response::write(std::string content) {
  if (!header_out) {
    header_out = true;
    std::string res_headers = "HTTP/1.0 200 OK\r\n";
    send(s, res_headers.data(), res_headers.size(), 0);
    for (auto h : headers) {
      auto hh = h.first + ": " + h.second + "\r\n";
      send(s, hh.data(), hh.size(), 0);
    }
    send(s, "\r\n", 2, 0);
  }
  send(s, content.data(), content.size(), 0);
}

struct request {
  std::string method;
  std::string uri;
  std::string raw_uri;
  std::vector<header> headers;
  std::map<std::string, std::string> uri_params;
  std::string body;

  request(
      std::string method, std::string raw_uri, std::string uri,
      std::map<std::string, std::string> uri_params,
      std::vector<header> headers, std::string body)
    : method(method), raw_uri(std::move(raw_uri)),
      uri(std::move(uri)), uri_params(std::move(uri_params)),
      headers(std::move(headers)), body(std::move(body)) { }
};

typedef std::function<void(response&, request&)> functor;
typedef std::function<std::string(request&)> functor_string;

class server_t {
private:
  std::vector<std::pair<std::string, functor>> handlers;
  std::vector<std::pair<std::string, functor_string>> handlers_string;
  logger log;

public:
  void GET(std::string path, functor fn);
  void POST(std::string path, functor fn);
  void GET(std::string path, functor_string fn);
  void POST(std::string path, functor_string fn);
  void run();
};

void server_t::GET(std::string path, functor fn) {
  handlers.push_back(std::make_pair(path, fn));
}

void server_t::POST(std::string path, functor fn) {
  handlers.push_back(std::make_pair(path, fn));
}

void server_t::GET(std::string path, functor_string fn) {
  handlers_string.push_back(std::make_pair(path, fn));
}

void server_t::POST(std::string path, functor_string fn) {
  handlers_string.push_back(std::make_pair(path, fn));
}

void server_t::run() {
  int server_fd, s; 
  struct sockaddr_in address; 
#ifdef _WIN32
  char opt = 1; 
#else
  int opt = 1; 
#endif
  int addrlen = sizeof(address); 

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 0), &wsa);
#endif

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
    throw std::runtime_error("socket failed");
  } 

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, (int) sizeof(opt))) { 
    throw std::runtime_error("setsockopt");
  } 
  address.sin_family = AF_INET; 
  address.sin_addr.s_addr = INADDR_ANY; 
  address.sin_port = htons(8080); 

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { 
    throw std::runtime_error("bind failed");
  } 
  if (listen(server_fd, 0) < 0) { 
    throw std::runtime_error("listen");
  } 
  while (true) {
    if ((s = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0) { 
      throw std::runtime_error("accept");
    }
    char buf[4096];
    const char *method, *path;
    int pret, minor_version;
    struct phr_header headers[100];
    size_t buflen = 0, prevbuflen = 0, method_len, path_len, num_headers;
    ssize_t rret;

    while (1) {
      while ((rret = recv(s, buf + buflen, sizeof(buf) - buflen, 0)) == -1 && errno == EINTR);
      if (rret <= 0) {
        // IOError
        continue;
      }

      prevbuflen = buflen;
      buflen += rret;
      num_headers = sizeof(headers) / sizeof(headers[0]);
      pret = phr_parse_request(
          buf, buflen, &method, &method_len, &path, &path_len,
          &minor_version, headers, &num_headers, prevbuflen);
      if (pret > 0) {
        break;
      }
      if (pret == -1) {
        // ParseError
        continue;
      }
      if (buflen == sizeof(buf)) {
        // RequestIsTooLongError
        continue;
      }
    }

    std::string req_method(method, method_len);
    std::string req_path(path, path_len);
    std::string req_body(buf + pret, buflen - pret);

    std::istringstream iss(req_path);
    std::map<std::string, std::string> req_uri_params;
    std::vector<header> req_headers;

    if (std::getline(iss, req_path, '?')) {
      std::string keyval, key, val;
      while(std::getline(iss, keyval, '&')) {
        std::istringstream isk(keyval);
        // TODO unescape query strings
        if(std::getline(std::getline(isk, key, '='), val))
          req_uri_params[key] = val;
      }
    }
    for (auto n = 0; n < num_headers; n++)
      req_headers.push_back(std::move(std::make_pair(
        std::move(std::string(headers[n].name, headers[n].name_len)),
        std::move(std::string(headers[n].value, headers[n].value_len)))));

    logger().get(INFO) << req_method << " " << req_path;

    request req(
        req_method,
        req_path,
        req_path,
        req_uri_params,
        req_headers,
        req_body);

    bool found = false;
    for (auto h : handlers_string) {
      if (h.first == req_path) {
        found = true;
        auto res = h.second(req);
        std::string res_headers = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n";
        send(s, res_headers.data(), res_headers.size(), 0);
        send(s, res.data(), res.size(), 0);
        break;
      }
    }
    if (!found) {
      for (auto h : handlers) {
        if (h.first == req_path) {
          found = true;
          response res(s);
          h.second(res, req);
          break;
        }
      }
    }
    if (!found) {
      std::string res_headers = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nnot found";
      send(s, res_headers.data(), res_headers.size(), 0);
    }
    closesocket(s);
  }
}

auto server() { return server_t{}; }

}

clask::log_level clask::logger::default_level = clask::INFO;

clask::log_level& clask::logger::level() {
  return default_level;
}