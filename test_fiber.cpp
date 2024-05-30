#include <iostream>

#include "include/fiber.h"
#include "include/thread.h"
#include "include/util.h"
void run_in_fiber() {
  static uint64_t id_r = Fiber::GetThis()->getId();
  spdlog::info("run in fiber {} begin", id_r);
  Fiber::GetThis()->yield();
  spdlog::info("run in fiber {} end", id_r);
}

void test_fiber() {
  // spdlog::info("");
  static uint64_t id_t = Fiber::GetThis()->getId();
  Fiber::GetThis();
  spdlog::info("fiber {} begin", id_t);
  Fiber::ptr fiber(new Fiber(run_in_fiber));
  fiber->resume();
  spdlog::info("fiber {} after", Fiber::GetThis()->getId());
  fiber->resume();
  spdlog::info("fiber {} end", id_t);
}

int main() {
  spdlog::set_pattern("[%c %z] [%^%l%$] [thread %t] %v");
  spdlog::set_level(spdlog::level::debug);  // Set global log level to debug
  spdlog::info("main begin");
  std::vector<Thread::ptr> thrs;
  for (int i = 0; i < 3; i++) {
    thrs.push_back(
        Thread::ptr(new Thread(&test_fiber, "Thread-" + std::to_string(i))));
  }

  for (auto i : thrs) {
    i->join();
  }
  spdlog::info("main end");
  return 0;
}