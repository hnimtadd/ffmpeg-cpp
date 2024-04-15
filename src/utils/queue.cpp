#include <mutex>
#include <utils/queue.h>
#include <iostream>

concurrent_counter::concurrent_counter() { this->value = 0; }
int concurrent_counter::get()
{
  std::lock_guard<std::mutex> lk(locker);
  return value++;
}

void concurrent_counter::reset()
{
  std::lock_guard<std::mutex> lk(locker);
  value = 0;
}

concurrent_logger::concurrent_logger(){};
void concurrent_logger::write(char *log)
{
  std::lock_guard lk(locker);
  log_queue.push(log);
  cv.notify_one();
}

char *concurrent_logger::read()
{
  std::unique_lock lk(locker);
  cv.wait(lk, [this]()
          { return !log_queue.empty(); });
  auto new_log = log_queue.front();
  log_queue.pop();
  lk.unlock();
  return new_log;
}