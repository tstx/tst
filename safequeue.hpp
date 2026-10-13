#pragma once

#include <cassert>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
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

// ---------------------------------------------------------------------------
// Test: ein Producer, zwei Consumer; jeder Wert muss genau einmal ankommen.
// ---------------------------------------------------------------------------

inline void testSafeQueue() {
  SafeQueue<int> q;
  long sum1 = 0, sum2 = 0;
  auto consume = [&q](long &sum) {
    while (auto v = q.pop())
      sum += *v;
  };
  std::thread c1(consume, std::ref(sum1));
  std::thread c2(consume, std::ref(sum2));

  for (int i = 1; i <= 1000; ++i)
    q.push(i);
  q.close();
  c1.join();
  c2.join();

  assert(sum1 + sum2 == 1000L * 1001 / 2);
  std::cout << "[SafeQueue] sum=" << sum1 + sum2 << " (c1=" << sum1
            << ", c2=" << sum2 << ")\n";
}
