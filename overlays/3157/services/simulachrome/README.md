# Simulachrome embedded VLM agent (scaffolding)

Architecture (do **not** put the 2GB model in the renderer):

```
Manager LLM (browser / glic)
  -> SimulachromeAgentHost::RunStep(task)
       -> VisualSettleTracker (200ms quiet)
       -> FrameCaptureClient -> Viz mailbox (SharedImage)
       -> Mojo SimulachromeAgent (utility process)
            -> smolvlm2 (llama.cpp/ORT)  [TODO]
            -> AgentAction {click|type|scroll, x,y in [0,1]}
       -> AgentInputInjector (DIP translate + ForwardMouseEvent)
```

## BUILD.gn modification checklist

1. **Mojo**
   - Already: `//services/simulachrome/public/mojom:mojom`

2. **Utility implementation**
   - Already: `//services/simulachrome:simulachrome_agent_service`

3. **Register in chrome utility** (`chrome/utility/services.cc`):
   ```cpp
   #include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"
   #include "services/simulachrome/simulachrome_agent_service.h"

   auto RunSimulachromeAgent(
       mojo::PendingReceiver<simulachrome::mojom::SimulachromeAgent> r) {
     return simulachrome::CreateSimulachromeAgent(
        std::move(r), content::UtilityThread::Get()->GetIOTaskRunner());
   }

   // inside RegisterMainThreadServices:
   services.Add(RunSimulachromeAgent);
   ```
   And add dep in `chrome/utility/BUILD.gn`:
   `//services/simulachrome:simulachrome_agent_service`

4. **Browser host**
   - Already: `//chrome/browser/simulachrome`
   - Add to `chrome/browser/BUILD.gn` (or dashboard target) deps:
     `//chrome/browser/simulachrome`

5. **Viz stub** (optional until capture lands)
   - Add sources under `components/viz/service` or a new
     `components/viz/service/simulachrome` source_set and dep from viz service.

6. **Model packaging**
   - Ship `smolvlm2` weights as a disc resource / component updater payload
     (~2GB). Pass path into `SimulachromeAgentHost::LoadModel`.
   - Do **not** link weights into `blink_renderer`.

7. **Sandbox note**
   - Default `[ServiceSandbox=kService]` cannot map GPU SharedImages.
   - Options later:
     a) GPU-process readback → SkBitmap over Mojo (simpler, copies), or
     b) Launch with a GPU-capable utility sandbox and pass SharedImageInterface.

## Quick smoke (once registered)

```cpp
auto host = std::make_unique<simulachrome::SimulachromeAgentHost>(web_contents);
host->RunStep("Click the submit button", base::BindOnce([](bool ok, auto err, auto action) {
  LOG(INFO) << "ok=" << ok << " err=" << err;
}));
```

Mock agent returns a center-click without a real model.


## Model weights (do NOT put in Chromium git)

Ollama stores content-addressed blobs under `$OLLAMA_MODELS/blobs/`.
On this machine: `OLLAMA_MODELS=/Users/matcauthon/0llamaModels`

| Role | Blob | Friendly export |
|------|------|-----------------|
| LLM (1.1GB) | `sha256-ead40252…` | `~/simulachrome_models/smolvlm2-agentic-gui.gguf` |
| mmproj (872MB) | `sha256-3296c1e6…` | `~/simulachrome_models/smolvlm2-mmproj.gguf` |

```bash
# re-export
third_party/llama_cpp/scripts/export_ollama_smolvlm2.sh ~/simulachrome_models

# load at runtime
host->LoadModel(base::FilePath("/Users/matcauthon/simulachrome_models"), ...);
```

## llama.cpp runtime

```bash
third_party/llama_cpp/scripts/clone_llama_cpp.sh
# then rebuild; services/simulachrome depends on //third_party/llama_cpp:llama_cpp_lib
```


## Off-screen targets (viewport strategies)

1. **Strategy 2 (preferred)** — Planner passes a CSS/A11y selector into
   `RunStep(instruction, selector)`. Browser checks
   `getBoundingClientRect()` vs the VisualViewport and, if needed, runs
   `scrollIntoView({behavior:'instant'})` *before* mailbox capture.
2. **Strategy 1** — smolvlm2 emits `scroll(down, 0.5)`; host injects a wheel
   event, re-settles (~200ms), re-captures, and re-runs the VLM (max 5 iters).
3. **Strategy 3** — after max scroll iters (or explicit model signal), host
   returns `AgentActionType::kElementNotFound` to the Planner over Mojo.
