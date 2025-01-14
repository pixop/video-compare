#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>

template <class T>
class Queue {
 protected:
  // Data
  std::queue<T> queue_;
  typename std::queue<T>::size_type size_max_;

  // Thread gubbins
  std::mutex mutex_;
  std::condition_variable full_;
  std::condition_variable empty_;

  std::atomic_bool quit_{false};
  std::atomic_bool stopped_{false};

 public:
  explicit Queue(size_t size_max);

  bool push(T&& data);
  bool push(const T& data);
  bool pop(T& data);

  void restart();
  void stop();
  void quit();

  // The queue has stopped accepting input
  bool is_stopped();
  bool is_quit();

  bool is_empty();
  void empty();
  int size();

 private:
  template <typename U>
  bool push_impl(U&& data);
};

template <class T>
Queue<T>::Queue(size_t size_max) : size_max_{size_max} {
  restart();
}

template <class T>
bool Queue<T>::push(T&& data) {
  return push_impl(std::move(data));
}

template <class T>
bool Queue<T>::push(const T& data) {
  return push_impl(data);
}

template <class T>
template <typename U>
bool Queue<T>::push_impl(U&& data) {
  std::unique_lock<std::mutex> lock(mutex_);

  while (!quit_ && !stopped_) {
    if (queue_.size() < size_max_) {
      queue_.push(std::forward<U>(data));

      empty_.notify_all();
      return true;
    }
    full_.wait(lock);
  }

  return false;
}

template <class T>
bool Queue<T>::pop(T& data) {
  std::unique_lock<std::mutex> lock(mutex_);

  while (!quit_) {
    if (!queue_.empty()) {
      data = std::move(queue_.front());
      queue_.pop();

      full_.notify_all();
      return true;
    }
    if (queue_.empty() && stopped_) {
      return false;
    }
    empty_.wait(lock);
  }

  return false;
}

template <class T>
void Queue<T>::restart() {
  std::unique_lock<std::mutex> lock(mutex_);

  stopped_ = false;
  empty_.notify_all();
  full_.notify_all();
}

template <class T>
void Queue<T>::stop() {
  std::unique_lock<std::mutex> lock(mutex_);

  stopped_ = true;
  empty_.notify_all();
}

template <class T>
bool Queue<T>::is_stopped() {
  return stopped_;
}

template <class T>
bool Queue<T>::is_quit() {
  return quit_;
}

template <class T>
void Queue<T>::quit() {
  std::unique_lock<std::mutex> lock(mutex_);

  quit_ = true;
  empty_.notify_all();
  full_.notify_all();
}

template <class T>
bool Queue<T>::is_empty() {
  return queue_.empty();
}

template <class T>
void Queue<T>::empty() {
  std::unique_lock<std::mutex> lock(mutex_);

  while (!queue_.empty()) {
    queue_.pop();
  }

  full_.notify_all();
}

template <class T>
int Queue<T>::size() {
  return queue_.size();
}
