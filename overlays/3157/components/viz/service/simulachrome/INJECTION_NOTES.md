# Viz / CC injection notes (Simulachrome)

## Current production path (landed)

Browser zero-copy capture does **not** require a custom Viz hook anymore:

`RenderWidgetHostView::{Mac,Aura}::CopySharedImageFromSurface`
  → `DelegatedFrameHost::CopyFromCompositingSurfaceAsTexture`
  → `CopyOutputRequest(RGBA, Destination::kSharedImage)`
  → `ClientSharedImage::{mailbox, creation_sync_token}`
  → `mojom::FrameTexture` over Mojo to the utility process

`FrameCaptureClient` retains the SharedImage until `ReleasePending()` after
`ExecuteAction` returns.

Intel Mac without Metal-for-llama: this path still works — compositing uses
the GPU; only the VLM stays CPU-side.

## Optional later: post-swap root-pass capture

If DFH copy is insufficient (e.g. need exact backbuffer after present):

### A. Visual activity → settle debounce
`cc/trees/layer_tree_host_impl.cc` — Scroll*/Animate → NotifyVisualActivity

### B. Capture after present
`components/viz/service/display/display.cc` — `DrawAndSwap` armed branch →
`VizFrameMailboxCapture::CaptureRootFrameAfterSwap`

### C. Browser bridge
New `viz.mojom.SimulachromeFrameCapture` for FrameSinkId → mailbox.

## Constraints
- Always pair Mailbox with SyncToken.
- Never run smolvlm2 in the Viz process.
- Keep capture off the critical path unless armed by `RunStep`.
