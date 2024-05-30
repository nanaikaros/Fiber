#include "scheduler.h"

#include <iostream>

#include "util.h"
/// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;
/// 当前线程的调度协程，每个线程都独有一份
static thread_local Fiber *t_scheduler_fiber = nullptr;

/**
 * @brief 创建调度器
 * @param[in] threads 线程数
 * @param[in] use_caller 是否将当前线程也作为调度线程
 * @param[in] name 名称
 */
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
    : m_useCaller(use_caller), m_name(name) {
  if (threads <= 0) {
    throw std::logic_error("create scheduler failed");
  }

  /**
    * 在user_caller为true的情况下，初始化caller线程的调度协程
    * caller线程的调度协程不会被调度器调度，⽽且，caller线程的调度协程停⽌时，应该返回caller线
   程的主协程
   */
  if (use_caller) {
    --threads;
    Fiber::GetThis();
    // 查看一下现在的线程有没有调度器
    if (Scheduler::GetThis() == nullptr) {
      t_scheduler = this;
    }

    // 调度协程
    m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

    Thread::SetName(m_name);
    // 当前线程的协调协程
    t_scheduler_fiber = m_rootFiber.get();

    m_rootThread = Util::GetThreadId();

    m_threadIds.push_back(m_rootThread);
  } else {
    m_rootThread = -1;
  }

  m_threadCount = threads;
}

// 获取当前线程的调度器
Scheduler *Scheduler::GetThis() { return t_scheduler; }

void Scheduler::setThis() { t_scheduler = this; }

Fiber *Scheduler::GetSchedulerFiber() { return t_scheduler_fiber; }

Scheduler::~Scheduler() {
  // SYLAR_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
  // SYLAR_ASSERT(m_stopping);
  if (m_stopping) {
    if (GetThis() == this) {
      t_scheduler = nullptr;
    }
  }
}

void Scheduler::start() {
  spdlog::info("Scheduler start...");
  MutexType::Lock lock(m_mutex);

  if (!m_stopping) {
    // std::cout << "scheduler stopping" << std::endl;
    return;
  }
  m_stopping = false;

  if (m_threads.empty()) {
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; i++) {
      m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                    m_name + "_" + std::to_string(i)));
      m_threadIds.push_back(m_threads[i]->getId());
    }
  }
  lock.unlock();
  // 执行调度协程
  // if (m_rootFiber) {
  //   spdlog::debug("Scheduler Fiber {} run", Fiber::GetThis()->getId());
  //   m_rootFiber->resume();
  // }
}

void Scheduler::run() {
  spdlog::info("Scheduler run. Now Fiber id {}", Fiber::GetThis()->getId());
  setThis();
  if (Util::GetThreadId() != m_rootThread) {
    t_scheduler_fiber = Fiber::GetThis().get();
  }

  Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
  Fiber::ptr cb_fiber;  // 回调函数

  ScheduleTask task;
  while (true) {
    task.reset();
    bool tickle_me = false;  // 是否tickle其他线程进⾏任务调度
    {
      MutexType::Lock lock(m_mutex);
      auto it = m_tasks.begin();
      while (it != m_tasks.end()) {
        if (it->thread != -1 && it->thread != Util::GetThreadId()) {
          // 指定了调度线程，但不是在当前线程上调度，标记⼀下需要通知其他线程进⾏调度，然后跳过这个任务，继续下⼀个
          ++it;
          tickle_me = true;  // 通知其他线程唤醒去完成
          continue;
        }
        // 找到⼀个未指定线程，或是指定了当前线程的任务
        // SYLAR_ASSERT(it->fiber || it->cb);
        if (it->fiber && it->fiber->getState() == Fiber::RUNNING) {
          // 任务队列时的协程⼀定是READY状态，谁会把RUNNING或TERM状态的协程加⼊调度呢？
          // SYLAR_ASSERT(it->fiber->getState() == Fiber::READY);
          ++it;
          continue;
        }
        // 当前调度线程找到⼀个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
        task = *it;
        m_tasks.erase(it++);
        ++m_activeThreadCount;
        break;
      }
    }
    // tickle_me |= (it != m_tasks.end());
    if (tickle_me) {
      tickle();
    }

    if (task.fiber && task.fiber->getState() != Fiber::TERM) {
      // resume协程，resume返回时，协程要么执⾏完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减⼀
      task.fiber->resume();
      --m_activeThreadCount;
      if (task.fiber && task.fiber->getState() == Fiber::READY) {
        schedule(task.fiber);
      }
      task.reset();
    } else if (task.cb) {
      if (cb_fiber) {
        cb_fiber->reset(task.cb);
      } else {
        cb_fiber.reset(new Fiber(task.cb));
      }
      task.reset();
      cb_fiber->resume();
      --m_activeThreadCount;
      cb_fiber.reset();
    } else {
      // 进到这个分⽀情况⼀定是任务队列空了，调度idle协程即可
      if (idle_fiber->getState() == Fiber::TERM) {
        // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那⼀定是调度器停⽌了
        // SYLAR_LOG_DEBUG(g_logger) << "idle fiber term";
        break;
      }
      ++m_idleThreadCount;
      idle_fiber->resume();
      --m_idleThreadCount;
    }
  }
  // SYLAR_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
}

void Scheduler::stop() {
  // SYLAR_LOG_DEBUG(g_logger) << "stop";
  // std::cout << "stop" << std::endl;
  if (m_rootFiber && m_threadCount == 0 &&
      m_rootFiber->getState() == Fiber::TERM) {
    spdlog::info("Scheduler stop");
    m_stopping = true;

    if (stopping()) {
      return;
    }
  }

  m_stopping = true;
  /// 如果use caller，那只能由caller线程发起stop
  if (m_useCaller) {
    // SYLAR_ASSERT(GetThis() == this);
  } else {
    // SYLAR_ASSERT(GetThis() != this);
  }
  for (size_t i = 0; i < m_threadCount; i++) {
    tickle();
  }
  if (m_rootFiber) {
    tickle();
  }
  /// 在use caller情况下，调度器协程结束时，应该返回caller协程
  if (m_rootFiber) {
    m_rootFiber->resume();
    // SYLAR_LOG_DEBUG(g_logger) << "m_rootFiber end";
  }
  std::vector<Thread::ptr> thrs;
  {
    MutexType::Lock lock(m_mutex);
    thrs.swap(m_threads);
  }
  for (auto &i : thrs) {
    i->join();
  }
}

void Scheduler::tickle() { spdlog::info("Tickle"); }

void Scheduler::idle() {
  spdlog::info("idle");
  // SYLAR_LOG_DEBUG(g_logger) << "idle";
  while (!stopping()) {
    Fiber::GetThis()->yield();
  }
}

bool Scheduler::stopping() {
  MutexType::Lock lock(m_mutex);
  return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}