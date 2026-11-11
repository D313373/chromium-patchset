// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/simulachrome/simulachrome_agent_service.h"

#include "services/simulachrome/simulachrome_agent_impl.h"

namespace simulachrome {

std::unique_ptr<mojom::SimulachromeAgent> CreateSimulachromeAgent(
    mojo::PendingReceiver<mojom::SimulachromeAgent> receiver,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner) {
  auto agent = std::make_unique<SimulachromeAgentImpl>(std::move(receiver));
  agent->SetIoTaskRunner(std::move(io_task_runner));
  return agent;
}

}  // namespace simulachrome
