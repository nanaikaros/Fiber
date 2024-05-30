#include "thread.h"

#include <iostream>
#include <stdexcept>

#include "util.h"
// 当前的线程指针
static thread_local Thread* t_thread = nullptr;
// 当前的线程名称
static thread_local std::string t_thread_name = "UNKNOW";

Thread* Thread::GetThis() { return t_thread; }

const std::string& Thread::GetName() const { return m_name; }

// Thread::Thread() { std::cout << "you should use another Thread" << std::endl;
// }

Thread::Thread(std::function<void()> cb, const std::string& name)
    : m_cb(cb), m_name(name) {
  if (name.empty()) m_name = "UNKONW";
  int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
  if (rt) {
    // SYLAR_LOG_ERROR(g_logger)
    //  << "pthread_create thread fail, rt=" << rt << " name=" << name;
    spdlog::error("Pthread create error");
    throw std::logic_error("pthread_create error");
  }
  m_semaphore.wait();
  spdlog::info("Thread create success, thread name: {}", m_name);
}

Thread::~Thread() {
  if (m_thread) {
    pthread_detach(m_thread);
  }
}

void Thread::join() {
  if (m_thread) {
    int rt = pthread_join(m_thread, nullptr);
    if (rt) {
      //   SYLAR_LOG_ERROR(g_logger)
      //       << "pthread_join thread fail, rt=" << rt << " name=" << m_name;
      spdlog::error("Pthread join error");
      throw std::logic_error("pthread_join error");
    }
    m_thread = 0;
  }
}

void Thread::SetName(const std::string& name) {
  if (name.empty()) {
    return;
  }
  if (t_thread) {
    t_thread->m_name = name;
  }
  t_thread_name = name;
}

void* Thread::run(void* arg) {
  Thread* thread = (Thread*)arg;
  t_thread = thread;
  t_thread_name = thread->m_name;
  thread->m_id = Util::GetThreadId();

  pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

  std::function<void()> cb;

  cb.swap(thread->m_cb);
  thread->m_semaphore.notify();
  cb();
  return 0;
}