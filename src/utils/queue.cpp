#include <mutex>
#include <utils/queue.h>

concurrent_counter::concurrent_counter() { this->value = 0; }
int concurrent_counter::get() {
  std::lock_guard<std::mutex> lk(locker);
  return value++;
}

void concurrent_counter::reset() {
  std::lock_guard<std::mutex> lk(locker);
  value = 0;
}
