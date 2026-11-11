#include "chrome/browser/ui/webui/dashboard/actions/native_locator_matches.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "ui/gfx/geometry/point_f.h"

namespace dashboard {

namespace {

constexpr size_t kMaxFrames = 24;
constexpr base::TimeDelta kReplyTimeout = base::Seconds(4);

// Specific locators first so broad TAG_NAME / CLASS_NAME sweeps cannot fill
// the per-frame cap before the selective ones run.
int LocatorSweepRank(LocatorType type) {
  switch (type) {
    case LocatorType::ID:
    case LocatorType::SHADOW_DOM:
      return 0;
    case LocatorType::CSS_SELECTOR:
    case LocatorType::NAME:
      return 1;
    case LocatorType::TEXT_CONTENT:
    case LocatorType::LINK_TEXT:
    case LocatorType::PARTIAL_LINK_TEXT:
    case LocatorType::ACCESSIBILITY_NAME:
      return 2;
    case LocatorType::CLASS_NAME:
      return 3;
    case LocatorType::TAG_NAME:
      return 4;
    default:
      return -1;
  }
}

base::Value::Dict ElementToPageMapValue(
    const mojom::ResolvedElementInfo& info,
    content::RenderFrameHost* rfh) {
  int x = info.x;
  int y = info.y;
  if (content::RenderWidgetHostView* view = rfh->GetView()) {
    const gfx::PointF root =
        view->TransformPointToRootCoordSpaceF(gfx::PointF(x, y));
    x = static_cast<int>(root.x());
    y = static_cast<int>(root.y());
  }
  base::Value::Dict bounds;
  bounds.Set("x", x);
  bounds.Set("y", y);
  bounds.Set("w", info.width);
  bounds.Set("h", info.height);

  base::Value::List locators;
  for (const auto& locator : info.locators) {
    base::Value::Dict entry;
    entry.Set("type", locator->type);
    entry.Set("value", locator->value);
    locators.Append(std::move(entry));
  }

  base::Value::Dict out;
  out.Set("bounds", std::move(bounds));
  out.Set("tag", info.tag);
  out.Set("text", info.text);
  out.Set("accessibilityName", info.accessibility_name);
  out.Set("role", info.role);
  out.Set("idAttr", info.id_attr);
  out.Set("classAttr", info.class_attr);
  out.Set("nameAttr", info.name_attr);
  out.Set("cssSelector", info.css);
  out.Set("xpath", info.xpath);
  out.Set("shadowChainSelector", info.shadow_chain_selector);
  out.Set("resourcePath", info.resource_path);
  out.Set("href", info.href);
  out.Set("src", info.src);
  out.Set("value", info.value);
  out.Set("visible", info.visible);
  out.Set("clickable", info.clickable);
  out.Set("focusable", info.focusable);
  out.Set("disabled", info.is_disabled);
  base::Value::Dict selectors;
  selectors.Set("locators", std::move(locators));
  out.Set("selectors", std::move(selectors));
  return out;
}

class CollectState : public base::RefCounted<CollectState> {
 public:
  explicit CollectState(base::OnceCallback<void(base::Value::Dict)> callback)
      : callback_(std::move(callback)) {}

  void AddPending() { ++pending_; }

  void AppendFrame(base::Value::Dict frame) {
    if (callback_) {
      frames_.Append(std::move(frame));
    }
  }

  void FrameDone() {
    if (pending_ > 0) {
      --pending_;
    }
    if (pending_ == 0) {
      Finish();
    }
  }

  void Finish() {
    if (!callback_) {
      return;
    }
    base::Value::Dict page_map;
    page_map.Set("frames", std::move(frames_));
    page_map.Set("source", "native_locator_matches");
    // Dropping the remotes releases the reply callbacks that hold `this`.
    remotes_.clear();
    std::move(callback_).Run(std::move(page_map));
  }

  std::vector<std::unique_ptr<
      mojo::AssociatedRemote<mojom::StealthyElementResolver>>>&
  remotes() {
    return remotes_;
  }

 private:
  friend class base::RefCounted<CollectState>;
  ~CollectState() = default;

  size_t pending_ = 0;
  base::Value::List frames_;
  base::OnceCallback<void(base::Value::Dict)> callback_;
  std::vector<
      std::unique_ptr<mojo::AssociatedRemote<mojom::StealthyElementResolver>>>
      remotes_;
};

}  // namespace

void CollectNativeLocatorMatches(
    content::WebContents* web_contents,
    const std::vector<Locator>& locators,
    mojom::SearchScope scope,
    int max_per_frame,
    base::OnceCallback<void(base::Value::Dict page_map)> callback) {
  std::vector<Locator> ordered;
  for (const Locator& locator : locators) {
    if (!locator.value.empty() && LocatorSweepRank(locator.type) >= 0) {
      ordered.push_back(locator);
    }
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Locator& a, const Locator& b) {
                     return LocatorSweepRank(a.type) < LocatorSweepRank(b.type);
                   });

  std::vector<content::RenderFrameHost*> frames;
  content::RenderFrameHost* primary =
      web_contents ? web_contents->GetPrimaryMainFrame() : nullptr;
  if (primary && !ordered.empty()) {
    content::Page* page = &primary->GetPage();
    web_contents->ForEachRenderFrameHost(
        [&frames, page](content::RenderFrameHost* rfh) {
          if (rfh && rfh->IsActive() && rfh->IsRenderFrameLive() &&
              &rfh->GetPage() == page && frames.size() < kMaxFrames) {
            frames.push_back(rfh);
          }
        });
  }

  auto state = base::MakeRefCounted<CollectState>(std::move(callback));
  // Held across the loop so a synchronous FrameDone cannot finish early.
  state->AddPending();
  for (content::RenderFrameHost* rfh : frames) {
    auto remote = std::make_unique<
        mojo::AssociatedRemote<mojom::StealthyElementResolver>>();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(remote.get());
    if (!remote->is_bound()) {
      continue;
    }
    std::vector<mojom::LocatorPtr> mojo_locators;
    for (const Locator& locator : ordered) {
      mojo_locators.push_back(mojom::Locator::New(
          LocatorTypeToString(locator.type), locator.value));
    }
    state->AddPending();
    auto done = std::make_shared<bool>(false);
    remote->set_disconnect_handler(base::BindOnce(
        [](CollectState* s, std::shared_ptr<bool> done) {
          if (!*done) {
            *done = true;
            s->FrameDone();
          }
        },
        base::Unretained(state.get()), done));
    (*remote)->ResolveAllMatches(
        std::move(mojo_locators), scope,
        static_cast<uint32_t>(std::max(1, max_per_frame)),
        base::BindOnce(
            [](scoped_refptr<CollectState> s,
               content::GlobalRenderFrameHostId frame_id,
               std::string frame_token, std::shared_ptr<bool> done,
               std::vector<mojom::ResolvedElementInfoPtr> elements) {
              if (*done) {
                return;
              }
              *done = true;
              content::RenderFrameHost* frame =
                  content::RenderFrameHost::FromID(frame_id);
              base::Value::List list;
              if (frame) {
                for (const auto& info : elements) {
                  if (info && info->ok) {
                    list.Append(ElementToPageMapValue(*info, frame));
                  }
                }
              }
              if (!list.empty()) {
                base::Value::Dict entry;
                entry.Set("frameId", frame_token);
                entry.Set("elements", std::move(list));
                s->AppendFrame(std::move(entry));
              }
              s->FrameDone();
            },
            state, rfh->GetGlobalId(), rfh->GetFrameToken().ToString(),
            done));
    state->remotes().push_back(std::move(remote));
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&CollectState::Finish, state), kReplyTimeout);
  state->FrameDone();
}

}  // namespace dashboard
