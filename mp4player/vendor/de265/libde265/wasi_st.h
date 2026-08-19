/*
 * Single-threaded std::thread/mutex shim for wasm32-wasi builds.
 *
 * wasi-sdk's libc++ is built without thread support (_LIBCPP_HAS_NO_THREADS):
 * <mutex>/<condition_variable>/<thread> exist but define no std::mutex etc.
 * The decoder runs strictly single-threaded inside the wapp (worker threads
 * are never started — de265_start_worker_threads() is not called), so every
 * lock is uncontended and every wait condition is already satisfied when
 * reached. These no-op stand-ins keep the unmodified libde265 sources
 * compiling. Local patch — not upstream libde265 code.
 */
#ifndef DE265_WASI_ST_H
#define DE265_WASI_ST_H

#if defined(__wasi__)

namespace std {

class mutex {
public:
  void lock() {}
  void unlock() {}
  bool try_lock() { return true; }
};

template <class M>
class unique_lock {
public:
  unique_lock() : m_(nullptr) {}
  explicit unique_lock(M& m) : m_(&m) { m.lock(); }
  ~unique_lock() { if (m_) m_->unlock(); }
  void lock() { if (m_) m_->lock(); }
  void unlock() { if (m_) m_->unlock(); }
  unique_lock(const unique_lock&) = delete;
  unique_lock& operator=(const unique_lock&) = delete;
private:
  M* m_;
};

class condition_variable {
public:
  // Single-threaded: a wait that would block can never be satisfied by
  // another thread — callers only reach wait() when the condition already
  // holds (worker pool is never started), so waiting is a no-op.
  void wait(unique_lock<mutex>&) {}
  template <class Pred>
  void wait(unique_lock<mutex>& l, Pred p) { while (!p()) { (void)l; break; } }
  void notify_one() {}
  void notify_all() {}
};

class thread {
public:
  thread() = default;
  template <class F, class... A>
  explicit thread(F&&, A&&...) {} // never used: worker pool is not started
  bool joinable() const { return false; }
  void join() {}
  void detach() {}
};

} // namespace std

#else
#include <mutex>
#include <condition_variable>
#include <thread>
#endif

#endif // DE265_WASI_ST_H
