#include <clask/core.hpp>
#include <ctime>
#include <unistd.h>

int main() {
  auto s = clask::server();

  s.GET("/", [&](clask::response_writer& w, clask::request& r) {
      w.code = 200;
      w.set_header("content-type", "text/event-stream; charset=utf-8");
      w.write_headers();

      clask::chunked_writer ww(w);
      for (int n = 0; n < 100; n++) {
        ww.write("💩");
        usleep(100000);
      }
      ww.end();
  });
  s.run();
}
