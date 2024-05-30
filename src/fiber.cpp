/**
 * @file fiber.cpp
 * @brief 协程实现
 * @version 0.1
 * @date 2021-06-15
 */

#include "fiber.h"

#include <atomic>
#include <iostream>

#include "scheduler.h"
// #include "config.h"

// static Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/// 全局静态变量，用于生成协程id
static std::atomic<uint64_t> s_fiber_id{0};
/// 全局静态变量，用于统计当前的协程数
static std::atomic<uint64_t> s_fiber_count{0};

/// 线程局部变量，当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;
/// 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程中运行，智能指针形式
static thread_local Fiber::ptr t_thread_fiber = nullptr;

// 协程栈大小，可通过配置文件获取，默认128k
//  static ConfigVar<uint32_t>::ptr g_fiber_stack_size =
//      Config::Lookup<uint32_t>("fiber.stack_size", 128 * 1024, "fiber stack
//      size");

/**
 * @brief malloc栈内存分配器
 */
class MallocStackAllocator {
 public:
  static void *Alloc(size_t size) { return malloc(size); }
  static void Dealloc(void *vp, size_t size) { return free(vp); }
};

using StackAllocator = MallocStackAllocator;

uint64_t Fiber::GetFiberId() {
  if (t_fiber) {
    return t_fiber->getId();
  }
  return 0;
}

Fiber::Fiber() {
  SetThis(this);
  m_state = RUNNING;

  if (getcontext(&m_ctx)) {
    // todo
    // SYLAR_ASSERT2(false, "getcontext");
  }

  ++s_fiber_count;
  m_id = s_fiber_id++;  // 协程id从0开始，用完加1
  spdlog::info("Fiber::main id = {}", m_id);

  // std::cout << "Fiber::Fiber() main id = " << m_id << std::endl;
}

void Fiber::SetThis(Fiber *f) { t_fiber = f; }

/**
 * 获取当前协程，同时充当初始化当前线程主协程的作用，这个函数在使用协程之前要调用一下
 */
Fiber::ptr Fiber::GetThis() {
  if (t_fiber) {
    return t_fiber->shared_from_this();
  }

  Fiber::ptr main_fiber(new Fiber);
  // SYLAR_ASSERT(t_fiber == main_fiber.get());
  t_thread_fiber = main_fiber;
  return t_fiber->shared_from_this();
}

/**
 * @brief 构造函数，⽤于创建⽤户协程
 * @param[] cb 协程⼊⼝函数
 * @param[] stacksize 栈⼤⼩，默认为128k
 */
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_id(s_fiber_id++), m_cb(cb), m_runInScheduler(run_in_scheduler) {
  ++s_fiber_count;
  m_stacksize = 128 * 1024;  // 默认128k
  m_stack = StackAllocator::Alloc(m_stacksize);

  if (getcontext(&m_ctx)) {
    // SYLAR_ASSERT2(false, "getcontext");
  }

  m_ctx.uc_link = nullptr;
  m_ctx.uc_stack.ss_sp = m_stack;
  m_ctx.uc_stack.ss_size = m_stacksize;

  makecontext(&m_ctx, &Fiber::MainFunc, 0);
  spdlog::info("Fiber id = {}", m_id);
  // std::cout << "Fiber::Fiber() id = " << m_id << std::endl;
}

/**
 * 线程的主协程析构时需要特殊处理，因为主协程没有分配栈和cb
 */
Fiber::~Fiber() {
  spdlog::info("~Fiber id = {}", m_id);
  // std::cout << "Fiber::~Fiber() id = " << m_id << std::endl;
  --s_fiber_count;
  if (m_stack) {
    // 有栈，说明是子协程，需要确保子协程一定是结束状态
    if (m_state == TERM) {
      StackAllocator::Dealloc(m_stack, m_stacksize);
    }
    spdlog::info("Dealloc stack, id = {}", m_id);
    // std::cout << "dealloc stack, id = " << m_id << std::endl;
  } else {
    // 没有栈，说明是线程的主协程
    // 主协程没有cb // 主协程一定是执行状态
    if (!m_cb && m_state == RUNNING) {
      Fiber *cur = t_fiber;  // 当前协程就是自己
      if (cur == this) {
        SetThis(nullptr);
      }
    }
  }
}

/**
 * 这里为了简化状态管理，强制只有TERM状态的协程才可以重置，但其实刚创建好但没执行过的协程也应该允许重置的
 */
void Fiber::reset(std::function<void()> cb) {
  // SYLAR_ASSERT(m_stack);
  if (m_state == TERM) {
    m_cb = cb;
    if (getcontext(&m_ctx)) {
      // SYLAR_ASSERT2(false, "getcontext");
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY;
  }
}

// 切换到当前协程执行
void Fiber::resume() {
  // SYLAR_ASSERT(m_state != TERM && m_state != RUNNING);
  if (m_state == READY) {
    SetThis(this);
    m_state = RUNNING;

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
      if (swapcontext(&(Scheduler::GetSchedulerFiber()->m_ctx), &m_ctx)) {
        // todo
      }
    } else {
      if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
        // todo
      }
    }
  }
}

void Fiber::yield() {
  /// 协程运行完之后会自动yield一次，用于回到主协程，此时状态已为结束状态
  if (m_state == RUNNING || m_state == TERM) {
    // SetThis(t_thread_fiber.get());
    if (m_state != TERM) {
      m_state = READY;
    }

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
      if (swapcontext(&m_ctx, &(Scheduler::GetSchedulerFiber()->m_ctx))) {
        // todo
        SetThis(Scheduler::GetSchedulerFiber());
      }
    } else {
      if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
        // todo
        SetThis(t_thread_fiber.get());
      }
    }
  }
}

/**
 * 这里没有处理协程函数出现异常的情况，同样是为了简化状态管理，
 * 并且个人认为协程的异常不应该由框架处理，应该由开发者自行处理
 */
void Fiber::MainFunc() {
  Fiber::ptr cur = GetThis();  // GetThis()的shared_from_this()方法让引用计数加1
  // SYLAR_ASSERT(cur);
  assert(cur);
  cur->m_cb();
  cur->m_cb = nullptr;
  cur->m_state = TERM;

  auto raw_ptr = cur.get();
  cur.reset();  // 手动让t_fiber的引用计数减1
  raw_ptr->yield();
}
