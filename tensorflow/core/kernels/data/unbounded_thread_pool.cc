/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/kernels/data/unbounded_thread_pool.h"

#include "absl/memory/memory.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/unbounded_work_queue.h"

namespace tensorflow {
namespace data {

// A lightweight wrapper for creating logical threads in a `UnboundedThreadPool`
// that can be shared (e.g.) in an `IteratorContext`.
class UnboundedThreadPool::LogicalThreadFactory : public ThreadFactory {
 public:
  explicit LogicalThreadFactory(UnboundedThreadPool* pool) : pool_(pool) {}

  std::unique_ptr<Thread> StartThread(const string& name,
                                      std::function<void()> fn) override {
    return pool_->ScheduleOnWorkQueue(std::move(fn));
  }

 private:
  UnboundedThreadPool* const pool_;  // Not owned.
};

// A logical implementation of the `tensorflow::Thread` interface that uses
// physical threads in an `UnboundedThreadPool` to perform the work.
//
// NOTE: This object represents a logical thread of control that may be mapped
// onto the same physical thread as other work items that are submitted to the
// same `UnboundedThreadPool`.
class UnboundedThreadPool::LogicalThreadWrapper : public Thread {
 public:
  explicit LogicalThreadWrapper(std::shared_ptr<Notification> join_notification)
      : join_notification_(std::move(join_notification)) {}

  ~LogicalThreadWrapper() override {
    // NOTE: The `Thread` destructor is expected to "join" the created thread,
    // but the physical thread may continue to execute after the work for this
    // thread is complete. We simulate this by waiting on a notification that
    // the thread's work function will notify when it is complete.
    join_notification_->WaitForNotification();
  }

 private:
  std::shared_ptr<Notification> join_notification_;
};

std::shared_ptr<ThreadFactory> UnboundedThreadPool::get_thread_factory() {
  return std::make_shared<LogicalThreadFactory>(this);
}

namespace {
void WorkQueueFunc(const std::function<void()>& fn,
                   std::shared_ptr<Notification> notification) {
  fn();
  notification->Notify();
}
}  // namespace

std::unique_ptr<Thread> UnboundedThreadPool::ScheduleOnWorkQueue(
    std::function<void()> fn) {
  auto join_notification = std::make_shared<Notification>();
  unbounded_work_queue_.Schedule(
      std::bind(&WorkQueueFunc, std::move(fn), join_notification));
  return absl::make_unique<LogicalThreadWrapper>(std::move(join_notification));
}

}  // namespace data
}  // namespace tensorflow
