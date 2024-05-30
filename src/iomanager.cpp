#include "iomanager.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <iostream>
IOManager::IOManager(size_t threads, bool use_caller, const std::string& name)
    : Scheduler(threads, use_caller, name) {
  m_epfd = epoll_create(5000);
  assert(m_epfd > 0);
  //  创建pipe，获取m_tickleFds[2]，其中m_tickleFds[0]是管道的读端，m_tickleFds[1]是管道的写端
  int rt = pipe(m_tickleFds);
  assert(!rt);
  // 注册pipe读句柄的可读事件，⽤于tickle调度协程，通过epoll_event.data.fd保存描述符
  epoll_event event;
  memset(&event, 0, sizeof(epoll_event));
  event.events = EPOLLIN | EPOLLET;
  event.data.fd = m_tickleFds[0];
  // ⾮阻塞⽅式，配合边缘触发
  rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
  assert(!rt);
  // 将管道的读描述符加⼊epoll多路复⽤，如果管道可读，idle中的epoll_wait会返回
  rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
  assert(!rt);

  contextResize(32);
  // 这⾥直接开启了Schedluer，也就是说IOManager创建即可调度协程
  start();
}

IOManager::~IOManager() {
  stop();
  close(m_epfd);
  close(m_tickleFds[0]);
  close(m_tickleFds[1]);

  for (size_t i = 0; i < m_fdContexts.size(); i++) {
    if (m_fdContexts[i]) {
      delete m_fdContexts[i];
    }
  }
}

void IOManager::contextResize(size_t size) {
  m_fdContexts.resize(size);
  for (size_t i = 0; i < m_fdContexts.size(); ++i) {
    if (!m_fdContexts[i]) {
      m_fdContexts[i] = new FdContext;
      m_fdContexts[i]->fd = i;
    }
  }
}

/**
 * @brief 添加事件
 * @details fd描述符发⽣了event事件时执⾏cb函数
 * @param[in] fd socket句柄
 * @param[in] event 事件类型
 * @param[in] cb 事件回调函数，如果为空，则默认把当前协程作为回调执⾏体
 * @return 添加成功返回0,失败返回-1
 */
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
  // 找到fd对应的FdContext，如果不存在，那就分配⼀个
  FdContext* fd_ctx = nullptr;
  RWMutexType::ReadLock lock(m_mutex);

  if ((int)m_fdContexts.size() > fd) {
    fd_ctx = m_fdContexts[fd];
    lock.unlock();
  } else {
    lock.unlock();
    RWMutexType::WriteLock lock2(m_mutex);
    contextResize(fd * 1.5);
    fd_ctx = m_fdContexts[fd];
  }

  FdContext::MutexType::Lock lock2(fd_ctx->mutex);
  // 同⼀个fd不允许重复添加相同的事件
  if (fd_ctx->events & event) {
  }

  int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  epoll_event epevent;
  epevent.events = EPOLLET | fd_ctx->events | event;
  epevent.data.ptr = fd_ctx;

  int rt = epoll_ctl(m_epfd, op, fd, &epevent);
  if (rt) {
    return -1;
  }

  ++m_pendingEventCount;

  // 找到这个fd的event事件对应的EventContext，对其中的scheduler, cb,
  // fiber进⾏赋值
  fd_ctx->events = (Event)(fd_ctx->events | event);
  FdContext::EventContext& event_ctx = fd_ctx->getContext(event);

  // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执⾏体
  event_ctx.scheduler = Scheduler::GetThis();
  if (cb) {
    event_ctx.cb.swap(cb);
  } else {
    event_ctx.fiber = Fiber::GetThis();
  }

  return 0;
}

bool IOManager::delEvent(int fd, Event event) {
  RWMutexType::ReadLock lock(m_mutex);
  if ((int)m_fdContexts.size() <= fd) {
    return false;
  }
  FdContext* fd_ctx = m_fdContexts[fd];
  lock.unlock();

  FdContext::MutexType::Lock lock2(fd_ctx->mutex);
  if (!(fd_ctx->events & event)) {
    return false;
  }
  // 清除指定的事件，表示不关⼼这个事件了，如果清除之后结果为0，则从epoll_wait中删除该⽂件描述符
  Event new_events = (Event)(fd_ctx->events & ~event);
  int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = EPOLLET | new_events;
  epevent.data.ptr = fd_ctx;

  int rt = epoll_ctl(m_epfd, op, fd, &epevent);
  if (rt) {
    return false;
  }

  --m_pendingEventCount;

  // 重置该fd对应的event事件上下⽂
  fd_ctx->events = new_events;
  FdContext::EventContext& event_ctx = fd_ctx->getContext(event);
  fd_ctx->resetContext(event_ctx);
  return true;
}

bool IOManager::cancelEvent(int fd, Event event) {
  RWMutexType::ReadLock lock(m_mutex);
  if ((int)m_fdContexts.size() <= fd) {
    return false;
  }
  FdContext* fd_ctx = m_fdContexts[fd];
  lock.unlock();

  FdContext::MutexType::Lock lock2(fd_ctx->mutex);
  if (!(fd_ctx->events & event)) {
    return false;
  }
  // 清除指定的事件，表示不关⼼这个事件了，如果清除之后结果为0，则从epoll_wait中删除该⽂件描述符
  Event new_events = (Event)(fd_ctx->events & ~event);
  int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = EPOLLET | new_events;
  epevent.data.ptr = fd_ctx;

  int rt = epoll_ctl(m_epfd, op, fd, &epevent);
  if (rt) {
    return false;
  }

  fd_ctx->triggerEvent(event);
  --m_pendingEventCount;
  return true;
}
/**
 * @brief 取消所有事件
 * @details 所有被注册的回调事件在cancel之前都会被执⾏⼀次
 * @param[in] fd socket句柄
 * @return 是否删除成功
 */
bool IOManager::cancelAll(int fd) {
  RWMutexType::ReadLock lock(m_mutex);
  if ((int)m_fdContexts.size() <= fd) {
    return false;
  }
  FdContext* fd_ctx = m_fdContexts[fd];
  lock.unlock();

  FdContext::MutexType::Lock lock2(fd_ctx->mutex);
  if (!fd_ctx->events) {
    return false;
  }

  int op = EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = 0;
  epevent.data.ptr = fd_ctx;

  int rt = epoll_ctl(m_epfd, op, fd, &epevent);
  if (rt) {
    return false;
  }

  if (fd_ctx->events & READ) {
    fd_ctx->triggerEvent(READ);
    --m_pendingEventCount;
  }
  if (fd_ctx->events & WRITE) {
    fd_ctx->triggerEvent(WRITE);
    --m_pendingEventCount;
  }

  return true;
}

IOManager* IOManager::GetThis() {
  return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

IOManager::FdContext::EventContext& IOManager::FdContext::getContext(
    IOManager::Event event) {
  switch (event) {
    case IOManager::READ:
      return read;
    case IOManager::WRITE:
      return write;
    default:
      return error;
      // assert(getContext);
  }
}

void IOManager::FdContext::resetContext(EventContext& ctx) {
  ctx.scheduler = nullptr;
  ctx.fiber.reset();
  ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(IOManager::Event event) {
  assert(events & event);
  events = (Event)(events & ~event);
  EventContext& ctx = getContext(event);
  if (ctx.cb) {
    ctx.scheduler->schedule(&ctx.cb);
  } else {
    ctx.scheduler->schedule(&ctx.fiber);
  }
  ctx.scheduler = nullptr;
  return;
}

/**
* @brief 通知调度器有任务要调度
* @details
写pipe让idle协程从epoll_wait退出，待idle协程yield之后Scheduler::run就可以调度其他
任务
* 如果当前没有空闲调度线程，那就没必要发通知
*/
void IOManager::tickle() {
  // SYLAR_LOG_DEBUG(g_logger) << "tickle";
  if (!hasIdleThreads()) {
    return;
  }
  int rt = write(m_tickleFds[1], "T", 1);
  // SYLAR_ASSERT(rt == 1);
  assert(rt == 1);
}

bool IOManager::stopping() {
  uint64_t timeout = 0;
  return stopping(timeout);
}

/**
* @brief idle协程
* @details
对于IO协程调度来说，应阻塞在等待IO事件上，idle退出的时机是epoll_wait返回，对应的操作是
tickle或注册的IO事件就绪
* 调度器⽆调度任务时会阻塞idle协程上，对IO调度器⽽⾔，idle状态应该关注两件事，⼀是有没有新的调度任
务，对应Schduler::schedule()，
* 如果有新的调度任务，那应该⽴即退出idle状态，并执⾏对应的任务；⼆是关注当前注册的所有IO事件有没有触
发，如果有触发，那么应该执⾏
* IO事件对应的回调函数
*/
void IOManager::idle() {
  const uint64_t MAX_EVNETS = 256;
  epoll_event* events = new epoll_event[MAX_EVNETS]();
  std::shared_ptr<epoll_event> shared_events(
      events, [](epoll_event* ptr) { delete[] ptr; });

  while (true) {
    // 获取下⼀个定时器的超时时间，顺便判断调度器是否停⽌
    uint64_t next_timeout = 0;
    if (stopping(next_timeout)) {
      std::cout << "name= " << Scheduler::getName() << "idle stopping exit"
                << std::endl;
      break;
    }

    // 阻塞在epoll_wait上，等待事件发⽣
    int rt = 0;

    do {
      static const int MAX_TIMEOUT = 3000;
      if (next_timeout != ~0ull) {
        next_timeout =
            (int)next_timeout > MAX_TIMEOUT ? MAX_TIMEOUT : next_timeout;
      } else {
        next_timeout = MAX_TIMEOUT;
      }
      rt = epoll_wait(m_epfd, events, MAX_EVNETS, (int)next_timeout);
      if (rt < 0 && errno == EINTR) {
        continue;

        //   SYLAR_LOG_ERROR(g_logger)
        //       << "epoll_wait(" << m_epfd << ") (rt=" << rt << ") (errno=" <<
        //       errno
        //       << ") (errstr:" << strerror(errno) << ")";
      } else {
        break;
      }
    } while (true);

    // 收集所有已超时的定时器，执⾏回调函数
    std::vector<std::function<void()>> cbs;
    listExpiredCb(cbs);
    if (!cbs.empty()) {
      // schedule(cbs.begin(), cbs.end());
      for (const auto& cb : cbs) {
        schedule(cb);
      }
      cbs.clear();
    }

    // 遍历所有发⽣的事件，根据epoll_event的私有指针找到对应的FdContext，进⾏事件处理
    for (int i = 0; i < rt; ++i) {
      epoll_event& event = events[i];
      if (event.data.fd == m_tickleFds[0]) {
        // ticklefd[0]⽤于通知协程调度，这时只需要把管道⾥的内容读完即可
        // 本轮idle结束Scheduler::run会重新执⾏协程调度
        uint8_t dummy[256];
        while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
        continue;
      }

      // 通过epoll_event的私有指针获取FdContext
      FdContext* fd_ctx = (FdContext*)event.data.ptr;
      FdContext::MutexType::Lock lock(fd_ctx->mutex);
      /**
       * EPOLLERR: 出错，⽐如写读端已经关闭的pipe
       * EPOLLHUP: 套接字对端关闭
       * 出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执⾏不到的情况
       */
      if (event.events & (EPOLLERR | EPOLLHUP)) {
        event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
      }
      int real_events = NONE;
      if (event.events & EPOLLIN) {
        real_events |= READ;
      }
      if (event.events & EPOLLOUT) {
        real_events |= WRITE;
      }
      if ((fd_ctx->events & real_events) == NONE) {
        continue;
      }

      // 剔除已经发⽣的事件，将剩下的事件重新加⼊epoll_wait，
      // 如果剩下的事件为0，表示这个fd已经不需要关注了，直接从epoll中删除
      int left_events = (fd_ctx->events & ~real_events);
      int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
      event.events = EPOLLET | left_events;

      int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
      if (rt2) {
        continue;
      }

      // 处理已经发⽣的事件，也就是让调度器调度指定的函数或协程
      if (real_events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
      }
      if (real_events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
      }
    }

    /**
     * ⼀旦处理完所有的事件，idle协程yield，这样可以让调度协程(Scheduler::run)
     * 重新检查是否有新任务要调度
     * 上⾯triggerEvent实际也只是把对应的fiber重新加⼊调度，要执⾏的话还要等idle协程退出
     */
    Fiber::ptr cur = Fiber::GetThis();
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();
  }
}

void IOManager::onTimerInsertedAtFront() { tickle(); }

bool IOManager::stopping(uint64_t& timeout) {
  timeout = getNextTimer();
  return timeout == 0 && m_pendingEventCount == 0 && Scheduler::stopping();
}