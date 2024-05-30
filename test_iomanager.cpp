#include <arpa/inet.h>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <vector>

#include "include/iomanager.h"
#include "include/util.h"

void test_fiber() { std::cout << "test fiber" << std::endl; }

void test1() {
  IOManager iom;
  iom.schedule(&test_fiber);

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  fcntl(sock, F_SETFL, O_NONBLOCK);

  sockaddr_in addr;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8000);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);

  iom.addEvent(sock, IOManager::WRITE,
               []() { std::cout << "connect" << std::endl; });
  connect(sock, (const sockaddr*)&addr, sizeof(addr));
}

Timer::ptr s_timer;
void test_timer() {
  IOManager iom(2);
  s_timer = iom.addTimer(
      1000,
      []() {
        spdlog::debug("hello timer");
        static int i = 0;
        if (++i == 3) {
          s_timer->cancel();
        }
      },
      true);
}

int main(int argc, char** argv) {
  spdlog::set_pattern("[%c %z] [%^%l%$] [thread %t] %v");
  spdlog::set_level(spdlog::level::debug);  // Set global log level to debug
  // test1();
  test_timer();
  return 0;
}
