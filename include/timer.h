#pragma once

#include <memory.h>
#include <stdint.h>

#include <functional>
#include <set>

#include "thread.h"
class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> {
  friend class TimerManager;

 public:
  typedef std::shared_ptr<Timer> ptr;
  /**
   * @brief 取消定时器
   */
  bool cancel();
  /**
   * @brief 刷新设置定时器的执⾏时间
   */
  bool refresh();
  /**
   * @brief 重置定时器时间
   * @param[in] ms 定时器执⾏间隔时间(毫秒)
   * @param[in] from_now 是否从当前时间开始计算
   */
  bool reset(uint64_t ms, bool from_now);

 private:
  /**
   * @brief 构造函数
   * @param[in] ms 定时器执⾏间隔时间
   * @param[in] cb 回调函数
   * @param[in] recurring 是否循环
   * @param[in] manager 定时器管理器
   */
  Timer(uint64_t ms, std::function<void()> cb, bool recurring,
        TimerManager* manager);

  Timer(uint64_t next);

 private:
  /// 是否循环定时器
  bool m_recurring = false;
  /// 执⾏周期
  uint64_t m_ms = 0;
  /// 精确的执⾏时间
  uint64_t m_next = 0;
  /// 回调函数
  std::function<void()> m_cb;
  /// 定时器管理器
  TimerManager* m_manager = nullptr;

 private:
  /**
   * @brief 定时器⽐较仿函数
   */
  struct Comparator {
    /**
     * @brief ⽐较定时器的智能指针的⼤⼩(按执⾏时间排序)
     * @param[in] lhs 定时器智能指针
     * @param[in] rhs 定时器智能指针
     */
    bool operator()(const Timer::ptr& lhs, const Timer::ptr& rhs) const;
  };
};

class TimerManager {
  friend class Timer;

 public:
  typedef RWMutex RWMutexType;
  /**
   * @brief 构造函数
   */
  TimerManager();
  /**
   * @brief 析构函数
   */
  virtual ~TimerManager();

  /**
   * @brief 添加定时器
   * @param[in] ms 定时器执⾏间隔时间
   * @param[in] cb 定时器回调函数
   * @param[in] recurring 是否循环定时器
   */
  Timer::ptr addTimer(uint64_t ms, std::function<void()> cb,
                      bool recurring = false);

  /**
   * @brief 添加条件定时器
   * @param[in] ms 定时器执⾏间隔时间
   * @param[in] cb 定时器回调函数
   * @param[in] weak_cond 条件
   * @param[in] recurring 是否循环
   */
  Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb,
                               std::weak_ptr<void> weak_cond,
                               bool recurring = false);

  /**
   * @brief 到最近⼀个定时器执⾏的时间间隔(毫秒)
   */
  uint64_t getNextTimer();
  /**
   * @brief 获取需要执⾏的定时器的回调函数列表
   * @param[in] cbs 回调函数数组
   */
  void listExpiredCb(std::vector<std::function<void()>>& cbs);
  /**
   * @brief 是否有定时器
   */
  bool hasTimer();

 protected:
  /**
   * @brief 当有新的定时器插⼊到定时器的⾸部,执⾏该函数
   */
  virtual void onTimerInsertedAtFront() = 0;
  /**
   * @brief 将定时器添加到管理器中
   */
  void addTimer(Timer::ptr val, RWMutexType::WriteLock& lock);

 private:
  /**
   * @brief 检测服务器时间是否被调后了
   */
  bool detectClockRollover(uint64_t now_ms);

 private:
  /// Mutex
  RWMutexType m_mutex;
  /// 定时器集合
  std::set<Timer::ptr, Timer::Comparator> m_timers;

  /// 是否触发onTimerInsertedAtFront
  bool m_tickled = false;
  /// 上次执⾏时间
  uint64_t m_previouseTime = 0;
};