#include <iostream>

#include "include/fiber.h"
#include "include/mutex.h"
#include "include/scheduler.h"
#include "include/util.h"

/**
 * @brief 演示协程主动yield情况下应该如何操作
 */
void test_fiber1() {
  std::cout << "test_fiber1 begin" << std::endl;

  /**
   * 协程主动让出执行权，在yield之前，协程必须再次将自己添加到调度器任务队列中，
   * 否则yield之后没人管，协程会处理未执行完的逃逸状态，测试时可以将下面这行注释掉以观察效果
   */
  Scheduler::GetThis()->schedule(Fiber::GetThis());

  std::cout << "before test_fiber1 yield" << std::endl;
  Fiber::GetThis()->yield();
  std::cout << "after test_fiber1 yield" << std::endl;

  std::cout << "test_fiber1 end" << std::endl;
}

/**
 * @brief 演示协程睡眠对主程序的影响
 */
void test_fiber2() {
  // SYLAR_LOG_INFO(g_logger) << "test_fiber2 begin";
  std::cout << "test_fiber2 begin" << std::endl;
  /**
   * 一个线程同一时间只能有一个协程在运行，线程调度协程的本质就是按顺序执行任务队列里的协程
   * 由于必须等一个协程执行完后才能执行下一个协程，所以任何一个协程的阻塞都会影响整个线程的协程调度，这里
   * 睡眠的3秒钟之内调度器不会调度新的协程，对sleep函数进行hook之后可以改变这种情况
   */
  sleep(3);

  std::cout << "test_fiber2 end" << std::endl;
}

void test_fiber3() {
  std::cout << "test_fiber3 begin" << std::endl;
  std::cout << "test_fiber3 end" << std::endl;
}

void test_fiber5() {
  static int count = 0;

  std::cout << "test_fiber5 begin, i = " << count << std::endl;
  std::cout << "test_fiber5 end i = " << count << std::endl;

  count++;
}

/**
 * @brief 演示指定执行线程的情况
 */
void test_fiber4() {
  std::cout << "test_fiber4 begin" << std::endl;

  for (int i = 0; i < 3; i++) {
    Scheduler::GetThis()->schedule(test_fiber5, Util::GetThreadId());
  }

  std::cout << "test_fiber4 end" << std::endl;
}

void test_fiber() {
  // static int s_count = 5;
  uint64_t fiber_id = Fiber::GetThis()->getId();
  spdlog::debug("Test in fiber {}", fiber_id);
  // sleep(1);
  // while (--s_count >= 0) {
  //   Scheduler::GetThis()->schedule(&test_fiber);
  // }
}

int main(int argc, char** agrv) {
  spdlog::set_pattern("[%c %z] [%^%l%$] [thread %t] %v");
  spdlog::set_level(spdlog::level::debug);  // Set global log level to debug
  spdlog::info("Main begin");

  Scheduler sc(2);
  sc.schedule(&test_fiber);
  sc.start();
  sc.schedule(&test_fiber);
  sc.stop();
  spdlog::info("Main end");
  return 0;
}