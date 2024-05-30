#pragma once
#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "fiber.h"
#include "mutex.h"
#include "nocopyable.h"

class Thread : Noncopyable {
 public:
  typedef std::shared_ptr<Thread> ptr;

  Thread(std::function<void()> cb, const std::string& name);

  ~Thread();

  pid_t getId() const { return m_id; }

  const std::string& GetName() const;

  void join();

  static Thread* GetThis();

  static void SetName(const std::string& name);

 private:
  static void* run(void* arg);

 private:
  pid_t m_id = -1;
  // 线程结构
  pthread_t m_thread = 0;
  std::function<void()> m_cb;
  std::string m_name;
  Semaphore m_semaphore;
};