#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
namespace Util {
inline pid_t GetThreadId() { return syscall(SYS_gettid); }

inline uint64_t GetCurrentMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000ul + tv.tv_usec / 1000;
}
inline uint64_t GetCurrentUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 * 1000ul + tv.tv_usec / 1000;
}

}  // namespace Util
