/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/transport/connectivity_state.h"

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/exec_ctx.h"

namespace grpc_core {

TraceFlag grpc_connectivity_state_trace(false, "connectivity_state");

const char* ConnectivityStateName(grpc_connectivity_state state) {
  switch (state) {
    case GRPC_CHANNEL_IDLE:
      return "IDLE";
    case GRPC_CHANNEL_CONNECTING:
      return "CONNECTING";
    case GRPC_CHANNEL_READY:
      return "READY";
    case GRPC_CHANNEL_TRANSIENT_FAILURE:
      return "TRANSIENT_FAILURE";
    case GRPC_CHANNEL_SHUTDOWN:
      return "SHUTDOWN";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}

//
// AsyncConnectivityStateWatcherInterface
//

// A fire-and-forget class to asynchronously deliver a connectivity
// state notification to a watcher.
class AsyncConnectivityStateWatcherInterface::Notifier {
 public:
  Notifier(RefCountedPtr<AsyncConnectivityStateWatcherInterface> watcher,
           grpc_connectivity_state state, Combiner* combiner)
      : watcher_(std::move(watcher)), state_(state) {
    if (combiner != nullptr) {
      combiner->Run(
          GRPC_CLOSURE_INIT(&closure_, SendNotification, this, nullptr),
          GRPC_ERROR_NONE);
    } else {
      GRPC_CLOSURE_INIT(&closure_, SendNotification, this,
                        grpc_schedule_on_exec_ctx);
      ExecCtx::Run(DEBUG_LOCATION, &closure_, GRPC_ERROR_NONE);
    }
  }

 private:
  static void SendNotification(void* arg, grpc_error* /*ignored*/) {
    Notifier* self = static_cast<Notifier*>(arg);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
      gpr_log(GPR_INFO, "watcher %p: delivering async notification for %s",
              self->watcher_.get(), ConnectivityStateName(self->state_));
    }
    self->watcher_->OnConnectivityStateChange(self->state_);
    Delete(self);
  }

  RefCountedPtr<AsyncConnectivityStateWatcherInterface> watcher_;
  const grpc_connectivity_state state_;
  grpc_closure closure_;
};

void AsyncConnectivityStateWatcherInterface::Notify(
    grpc_connectivity_state state) {
  New<Notifier>(Ref(), state, combiner_);  // Deletes itself when done.
}

//
// ConnectivityStateTracker
//

ConnectivityStateTracker::~ConnectivityStateTracker() {
  grpc_connectivity_state current_state = state_.Load(MemoryOrder::RELAXED);
  if (current_state == GRPC_CHANNEL_SHUTDOWN) return;
  for (const auto& p : watchers_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
      gpr_log(GPR_INFO,
              "ConnectivityStateTracker %s[%p]: notifying watcher %p: %s -> %s",
              name_, this, p.first, ConnectivityStateName(current_state),
              ConnectivityStateName(GRPC_CHANNEL_SHUTDOWN));
    }
    p.second->Notify(GRPC_CHANNEL_SHUTDOWN);
  }
}

void ConnectivityStateTracker::AddWatcher(
    grpc_connectivity_state initial_state,
    OrphanablePtr<ConnectivityStateWatcherInterface> watcher) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
    gpr_log(GPR_INFO, "ConnectivityStateTracker %s[%p]: add watcher %p", name_,
            this, watcher.get());
  }
  grpc_connectivity_state current_state = state_.Load(MemoryOrder::RELAXED);
  if (initial_state != current_state) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
      gpr_log(GPR_INFO,
              "ConnectivityStateTracker %s[%p]: notifying watcher %p: %s -> %s",
              name_, this, watcher.get(), ConnectivityStateName(initial_state),
              ConnectivityStateName(current_state));
    }
    watcher->Notify(current_state);
  }
  // If we're in state SHUTDOWN, don't add the watcher, so that it will
  // be orphaned immediately.
  if (current_state != GRPC_CHANNEL_SHUTDOWN) {
    watchers_.insert(std::make_pair(watcher.get(), std::move(watcher)));
  }
}

void ConnectivityStateTracker::RemoveWatcher(
    ConnectivityStateWatcherInterface* watcher) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
    gpr_log(GPR_INFO, "ConnectivityStateTracker %s[%p]: remove watcher %p",
            name_, this, watcher);
  }
  watchers_.erase(watcher);
}

void ConnectivityStateTracker::SetState(grpc_connectivity_state state,
                                        const char* reason) {
  grpc_connectivity_state current_state = state_.Load(MemoryOrder::RELAXED);
  if (state == current_state) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
    gpr_log(GPR_INFO, "ConnectivityStateTracker %s[%p]: %s -> %s (%s)", name_,
            this, ConnectivityStateName(current_state),
            ConnectivityStateName(state), reason);
  }
  state_.Store(state, MemoryOrder::RELAXED);
  for (const auto& p : watchers_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
      gpr_log(GPR_INFO,
              "ConnectivityStateTracker %s[%p]: notifying watcher %p: %s -> %s",
              name_, this, p.first, ConnectivityStateName(current_state),
              ConnectivityStateName(state));
    }
    p.second->Notify(state);
  }
  // If the new state is SHUTDOWN, orphan all of the watchers.  This
  // avoids the need for the callers to explicitly cancel them.
  if (state == GRPC_CHANNEL_SHUTDOWN) watchers_.clear();
}

grpc_connectivity_state ConnectivityStateTracker::state() const {
  grpc_connectivity_state state = state_.Load(MemoryOrder::RELAXED);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_connectivity_state_trace)) {
    gpr_log(GPR_INFO, "ConnectivityStateTracker %s[%p]: get current state: %s",
            name_, this, ConnectivityStateName(state));
  }
  return state;
}

}  // namespace grpc_core
