#include <mutex>
#include <queue>

template <typename Data> class concurrent_queue {
private:
  std::queue<Data> the_queue;
  std::mutex the_mutex;
  std::condition_variable the_condition_variable;

public:
  void push(Data const &data) {
    std::lock_guard lock(the_mutex);
    the_queue.push(data);
    /* lock.unlock(); */
    the_condition_variable.notify_one();
  }

  bool empty() const {
    std::scoped_lock lock(the_mutex);
    return the_queue.empty();
  }

  bool try_pop(Data &popped_value) {
    std::scoped_lock lock(the_mutex);
    if (the_queue.empty()) {
      return false;
    }

    popped_value = the_queue.front();
    the_queue.pop();
    return true;
  }

  void wait_and_pop(Data &popped_value) {
    std::unique_lock lock(the_mutex);

    the_condition_variable.wait(lock, [this] { return !the_queue.empty(); });
    popped_value = the_queue.front();
    lock.unlock();
    the_queue.pop();
  }
};

class concurrent_counter {
private:
  std::mutex locker;
  int value;

public:
  concurrent_counter();
  int get();
  void reset();
};
