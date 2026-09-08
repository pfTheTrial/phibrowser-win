// Host-test shim for base/no_destructor.h. The real header is pure C++ but
// lives under a //base include root the host build does not have; the client's
// host-compilable .cc files (phi_theme.cc) use it for their immutable
// catalogs. This is the same contract — construct on first use, never
// destroy — in the twenty lines the tests need.
#ifndef PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_NO_DESTRUCTOR_H_
#define PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_NO_DESTRUCTOR_H_

#include <new>
#include <utility>

namespace base {

template <typename T>
class NoDestructor {
 public:
  explicit NoDestructor(const T& value) { new (storage_) T(value); }
  explicit NoDestructor(T&& value) { new (storage_) T(std::move(value)); }
  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    new (storage_) T(std::forward<Args>(args)...);
  }
  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;
  ~NoDestructor() = default;  // Deliberately does not destroy the T.

  const T& operator*() const { return *get(); }
  T& operator*() { return *get(); }
  const T* operator->() const { return get(); }
  T* operator->() { return get(); }
  const T* get() const { return reinterpret_cast<const T*>(storage_); }
  T* get() { return reinterpret_cast<T*>(storage_); }

 private:
  alignas(T) char storage_[sizeof(T)];
};

}  // namespace base

#endif  // PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_NO_DESTRUCTOR_H_
