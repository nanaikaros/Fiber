#pragma once

#include "mutex.h"
#include "scheduler.h"
#include "timer.h"
class IOManager : public Scheduler, public TimerManager {
 public:
  typedef std::shared_ptr<IOManager> ptr;
  typedef RWMutex RWMutexType;

  enum Event { NONE = 0X0, READ = 0X1, WRITE = 0X4 };

 private:
  struct FdContext {
    typedef Mutex MutexType;
    struct EventContext {
      Scheduler* scheduler = nullptr;
      Fiber::ptr fiber;
      std::function<void()> cb;
    };

    EventContext& getContext(Event event);
    void resetContext(EventContext& ctx);
    void triggerEvent(Event event);
    EventContext read;   // 读事件
    EventContext write;  // 写事件
    EventContext error;
    int fd = 0;           // 事件关联的句柄
    Event events = NONE;  // 已经注册的事件
    MutexType mutex;
  };

 public:
  IOManager(size_t threads = 1, bool use_caller = true,
            const std::string& name = "IOManager");
  ~IOManager();

  // 1 success 0 retry -1 error
  int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
  bool delEvent(int fd, Event event);
  bool cancelEvent(int fd, Event event);

  bool cancelAll(int fd);

  static IOManager* GetThis();

 protected:
  void tickle() override;
  void idle() override;
  bool stopping() override;
  bool stopping(uint64_t& timeout);
  void onTimerInsertedAtFront() override;

  void contextResize(size_t size);

 private:
  /// epoll ⽂件句柄
  int m_epfd = 0;
  /// pipe ⽂件句柄，fd[0]读端，fd[1]写端
  int m_tickleFds[2];
  /// 当前等待执⾏的IO事件数量
  std::atomic<size_t> m_pendingEventCount = {0};
  /// IOManager的Mutex
  RWMutexType m_mutex;
  /// socket事件上下⽂的容器
  std::vector<FdContext*> m_fdContexts;
};