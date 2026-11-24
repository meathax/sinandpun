#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "../thread.hpp"

namespace aurora::gfx::render_worker {

enum class ItemType : uint8_t {
  BeginFrame,
  EncodePass,
  EndFrame,
  Sync,
  Shutdown,
};

using WorkCallback = std::function<void()>;

struct SyncState {
  std::mutex mutex;
  std::condition_variable cv;
  bool complete = false;
};

struct QueueItem {
  ItemType type = ItemType::Sync;
  uint64_t frameId = 0;
  uint32_t passIndex = 0;
  WorkCallback work;
  std::shared_ptr<SyncState> sync;
};

class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity);

  bool push(QueueItem item);
  std::optional<QueueItem> pop_for(std::chrono::milliseconds timeout, bool& closed);
  void close();
  void reset();
  [[nodiscard]] size_t size() const;

private:
  size_t m_capacity = 0;
  mutable std::mutex m_mutex;
  std::condition_variable m_notEmpty;
  std::condition_variable m_notFull;
  std::deque<QueueItem> m_items;
  bool m_closed = false;
};

class FrameSlotPool {
public:
  explicit FrameSlotPool(size_t slotCount);

  size_t acquire();
  std::optional<size_t> try_acquire();
  void release(size_t slot);
  void reset();
  [[nodiscard]] size_t free_count() const;

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<bool> m_freeSlots;
};

// Worker-owned scene renderer. Retired before GPU submission changes its resources.
void set_idle_render(WorkCallback render, WorkCallback release);
void clear_idle_render();
void request_idle_retire();
void initialize();
void shutdown();

void enqueue_begin_frame(uint64_t frameId, WorkCallback work);
void enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work);
void enqueue_end_frame(uint64_t frameId, WorkCallback work);
void enqueue_work(WorkCallback work);
void synchronize();
// Like synchronize(), but gives up after `timeout`. Returns whether the worker caught up.
bool synchronize_for(std::chrono::microseconds timeout);

bool is_worker_thread() noexcept;
bool is_idle() noexcept;

using ProducerWaitCallback = void (*)();
void set_producer_wait_callback(ProducerWaitCallback callback) noexcept;
ProducerWaitCallback producer_wait_callback() noexcept;

// Runs the producer wait callback while the calling thread blocks on the renderer.
void service_producer_wait() noexcept;

// True while a newer frame than the one being encoded is already sealed and queued, or while a
// producer thread is blocked on the render worker. Interpolated slots of the current frame are then
// stale work that would hold the game back, so the encoder drops them (the game must never slow down
// to make room for inserted frames).
bool newer_frame_pending() noexcept;

// Marks a non-worker thread as blocked on the render worker for newer_frame_pending().
struct ProducerWaitScope {
  ProducerWaitScope() noexcept;
  ~ProducerWaitScope();
  ProducerWaitScope(const ProducerWaitScope&) = delete;
  ProducerWaitScope& operator=(const ProducerWaitScope&) = delete;
private:
  bool m_counted;
};

// cv.wait(lock, pred), but the producer wait callback keeps running every millisecond.
template <typename Pred>
void wait_servicing(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, Pred pred) {
  if (pred()) {
    return;
  }
  ProducerWaitScope waiting;
  if (producer_wait_callback() == nullptr || is_worker_thread()) {
    cv.wait(lock, pred);
    return;
  }
  while (!thread::wait_for(cv, lock, std::chrono::milliseconds{1}, pred)) {
    lock.unlock();
    service_producer_wait();
    lock.lock();
  }
}

} // namespace aurora::gfx::render_worker
