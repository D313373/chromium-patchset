// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_SIMULACHROME_SIMULACHROME_AGENT_SERVICE_H_
#define SERVICES_SIMULACHROME_SIMULACHROME_AGENT_SERVICE_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"

namespace simulachrome {

// Factory entrypoint used by chrome/utility RegisterMainThreadServices.
// |io_task_runner| is the utility IO thread. AttachGpu uses it to bind the
// viz::Gpu pipe. Null leaves mailbox readback disabled.
std::unique_ptr<mojom::SimulachromeAgent> CreateSimulachromeAgent(
    mojo::PendingReceiver<mojom::SimulachromeAgent> receiver,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner);

}  // namespace simulachrome

#endif  // SERVICES_SIMULACHROME_SIMULACHROME_AGENT_SERVICE_H_
