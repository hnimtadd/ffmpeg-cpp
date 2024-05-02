#include <mutex>
#include <queue>

const int DEFAULT_BUFFER = 100;
template <typename Data>
class concurrent_queue
{
private:
  std::queue<Data> the_queue;
  std::mutex the_mutex;
  std::condition_variable the_condition_variable;

public:
  void wait_and_push(Data const &data)
  {
    std::unique_lock lock(the_mutex);
    the_condition_variable.wait(lock, [this]
                                { return the_queue.size() < DEFAULT_BUFFER; });
    the_queue.push(data);
    the_condition_variable.notify_all();
    lock.unlock();
  }

  bool empty() const
  {
    std::scoped_lock lock(the_mutex);
    return the_queue.empty();
  }

  void wait_and_pop(Data &popped_value)
  {
    std::unique_lock lock(the_mutex);

    the_condition_variable.wait(lock, [this]
                                { return !the_queue.empty(); });
    popped_value = the_queue.front();
    the_queue.pop();
    lock.unlock();
    the_condition_variable.notify_all();
  }
};

class concurrent_counter
{
private:
  std::mutex locker;
  int value;

public:
  concurrent_counter();
  int get();
  void reset();
};

class concurrent_logger
{
private:
  std::mutex locker;
  std::queue<char *> log_queue;
  std::condition_variable cv;

public:
  concurrent_logger();
  void write(char *);
  char *read();
};
