#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

template <typename T> 
class SafeQueue {
  std::queue<T> q_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool closed_ = false; // durch m_ geschützt, muss nicht atomic sein

public:
  void push(T v) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push(std::move(v));
    }
    cv_.notify_one();
  }

  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
    }
    cv_.notify_all(); // ALLE Wartenden wecken
  }

  std::optional<T> pop() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return !q_.empty() || closed_; });
    if (q_.empty())
      return std::nullopt; // geschlossen und leer → Consumer beendet sich
    T v = std::move(q_.front());
    q_.pop();
    return v;
  }
};
