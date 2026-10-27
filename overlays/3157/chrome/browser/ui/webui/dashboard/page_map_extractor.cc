#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"

#include <utility>
#include <map>
// #include "chrome/browser/ui/webui/dashboard/page_map_extractor.h" // <-- Removed duplicate include

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/render_frame_host.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"

#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "url/gurl.h"

namespace {

// Helper small internal forward decls (implement or replace with your existing collectors)
// (no timeout needed in this stub)

// Synchronous placeholder: collect DOM / accessibility elements into extractor.elements_.
// Replace with a proper DOM walker if available.
void CollectElementsSync(dashboard::PageMapExtractor* extractor, content::WebContents* wc, bool greedy) {
  // TODO: Replace this with your DOM walker or stealth resolver result ingestion.
  // For now, assume extractor->elements_ was populated elsewhere (e.g. StealthyElementResolver)
  // or by callers. This function is a no-op fallback.
  (void)extractor;
  (void)wc;
  (void)greedy;
}

// Small helper to convert a gfx::RectF to a JSON dictionary.
base::Value::Dict RectToValue(const gfx::RectF& r) {
  base::Value::Dict out;
  out.Set("x", r.x());
  out.Set("y", r.y());
  out.Set("w", r.width());
  out.Set("h", r.height());
  return out;
}

base::Value::List LocatorBundleToList(
    const std::vector<dashboard::mojom::LocatorPtr>& locators) {
  base::Value::List list;
  for (const auto& l : locators) {
    base::Value::Dict d;
    d.Set("type", l->type);
    d.Set("value", l->value);
    list.Append(std::move(d));
  }
  return list;
}

dashboard::PageElementSnapshot ToSnapshot(
    dashboard::mojom::ResolvedElementInfoPtr info) {
  dashboard::PageElementSnapshot snap;
  snap.element_id = info->node_id ? base::NumberToString(info->node_id) : "";
  snap.frame_id = info->frame_id;
  snap.tag = info->tag;
  snap.text = info->text;
  snap.accessibility_name =
      !info->accessibility_name.empty() ? info->accessibility_name
                                        : info->aria_label;
  snap.role = info->role;
  snap.css_selector = info->css;
  snap.xpath = info->xpath;
  snap.locator_bundle = LocatorBundleToList(info->locators);
  snap.clickable = info->clickable;
  snap.focusable = info->focusable;
  snap.disabled = info->is_disabled;
  snap.bounds = gfx::RectF(info->x, info->y, info->width, info->height);
  return snap;
}

}  // namespace

// --- Added missing namespace opening ---
namespace dashboard {

// ---------------- PageElementSnapshot ----------------
PageElementSnapshot::PageElementSnapshot() = default;
// Copying is disabled due to move-only members.
PageElementSnapshot::PageElementSnapshot(PageElementSnapshot&&) noexcept = default;
// Copy assignment disabled.
PageElementSnapshot& PageElementSnapshot::operator=(PageElementSnapshot&&) noexcept = default;
PageElementSnapshot::~PageElementSnapshot() = default;

base::Value::Dict PageElementSnapshot::ToValueDict() const {
  base::Value::Dict dict;
  if (!element_id.empty()) dict.Set("id", element_id);
  if (!frame_id.empty()) dict.Set("frameId", frame_id);
  if (!tag.empty()) dict.Set("tag", tag);
  if (!role.empty()) dict.Set("role", role);
  if (!type.empty()) dict.Set("type", type);
  if (!text.empty()) dict.Set("text", text);
  if (!accessibility_name.empty()) dict.Set("accessibilityName", accessibility_name);
  if (!interactive_ancestor_role.empty()) {
    dict.Set("interactiveAncestorRole", interactive_ancestor_role);
  }
  if (!id_attr.empty()) dict.Set("idAttr", id_attr);
  if (!class_attr.empty()) dict.Set("classAttr", class_attr);
  if (!name_attr.empty()) dict.Set("nameAttr", name_attr);

  // Selector bundle.
  base::Value::Dict selectors;
  if (!css_selector.empty()) selectors.Set("css", css_selector);
  if (!xpath.empty()) selectors.Set("xpath", xpath);
  if (!shadow_chain_selector.empty()) selectors.Set("shadowChain", shadow_chain_selector);
  if (!resource_path.empty()) selectors.Set("resourcePath", resource_path);
  if (!locator_bundle.empty()) selectors.Set("locators", locator_bundle.Clone());
  if (!selectors.empty()) dict.Set("selectors", std::move(selectors));

  dict.Set("clickable", clickable);
  dict.Set("focusable", focusable);
  dict.Set("editable", editable);
  dict.Set("disabled", disabled);
  dict.Set("nonempty", field_nonempty);
  if (!placeholder.empty()) {
    dict.Set("placeholder", placeholder);
  }
  if (!field_value.empty()) {
    dict.Set("fieldValue", field_value);
  }
  dict.Set("inShadowDom", in_shadow_dom);

  if (!shadow_host_selector.empty())
    dict.Set("shadowHostSelector", shadow_host_selector);

  dict.Set("bounds", RectToValue(bounds));

  return dict;
}

// ---------------- PageMapExtractor ----------------
PageMapExtractor::FrameBucket::FrameBucket() = default;
PageMapExtractor::FrameBucket::FrameBucket(FrameBucket&&) noexcept = default;
// Copy operations are deleted due to move-only members.
PageMapExtractor::FrameBucket& PageMapExtractor::FrameBucket::operator=(FrameBucket&&) noexcept = default;
PageMapExtractor::FrameBucket::~FrameBucket() = default;

PageMapExtractor::PageMapExtractor() = default;
PageMapExtractor::~PageMapExtractor() = default;

void PageMapExtractor::AddElement(PageElementSnapshot element) {
  elements_.push_back(std::move(element));
}

base::Value::Dict PageMapExtractor::BuildPageMap() const {
  std::map<std::string, FrameBucket> frame_buckets;

  // 1) Group elements by frame and shadow host.
  for (const auto& elem : elements_) {
    const std::string frame_id = elem.frame_id.empty() ? "root" : elem.frame_id;
    FrameBucket& frame_bucket = frame_buckets[frame_id];
    frame_bucket.frame_id = frame_id;

    base::Value::Dict elem_value = elem.ToValueDict();

    if (!elem.in_shadow_dom || elem.shadow_host_selector.empty()) {
      frame_bucket.elements.Append(std::move(elem_value));
    } else {
      ShadowBucket& shadow_bucket =
          frame_bucket.shadow_buckets[elem.shadow_host_selector];
      shadow_bucket.host_selector = elem.shadow_host_selector;
      shadow_bucket.elements.Append(std::move(elem_value));
    }
  }

  // 2) Create "frames" array.
  base::Value::List frames;
  for (auto& [_, bucket] : frame_buckets) {
    base::Value::Dict frame_dict;
    frame_dict.Set("frameId", bucket.frame_id);

    if (url_.is_valid())
      frame_dict.Set("url", url_.spec());

    frame_dict.Set("elements", std::move(bucket.elements));

    if (!bucket.shadow_buckets.empty()) {
      base::Value::List shadow_roots;
      for (auto& [__, shadow_bucket] : bucket.shadow_buckets) {
        base::Value::Dict shadow_dict;
        shadow_dict.Set("hostSelector", shadow_bucket.host_selector);
        shadow_dict.Set("elements", std::move(shadow_bucket.elements));
        shadow_roots.Append(std::move(shadow_dict));
      }
      frame_dict.Set("shadowRoots", std::move(shadow_roots));
    }

    frames.Append(std::move(frame_dict));
  }

  // 3) Build metadata.
  std::size_t interactive_count = elements_.size();
  std::size_t shadow_count = 0;
  for (const auto& e : elements_) {
    if (e.in_shadow_dom) ++shadow_count;
  }

  base::Value::Dict metadata;
  metadata.Set("interactiveCount", static_cast<int>(interactive_count));
  metadata.Set("shadowDomElementCount", static_cast<int>(shadow_count));
  if (!focused_element_id_.empty())
    metadata.Set("focusedElementId", focused_element_id_);

  // 4) Final root object.
  base::Value::Dict root;
  if (url_.is_valid())
    root.Set("url", url_.spec());

  const int64_t now_ms = base::Time::Now().ToDeltaSinceWindowsEpoch().InMilliseconds();
  root.Set("timestamp", static_cast<double>(timestamp_ms_ == 0 ? now_ms : timestamp_ms_));

  root.Set("frames", std::move(frames));
  root.Set("metadata", std::move(metadata));

  return root;
}

// Forward declarations of async helpers
void RunDomCollectorAsync(PageMapExtractor* extractor,
                          content::WebContents* wc,
                          bool greedy,
                          base::OnceClosure done_cb);
void RunStealthResolverAsync(PageMapExtractor* extractor,
                             content::WebContents* wc,
                             bool greedy,
                             base::OnceClosure done_cb);

// Thunks to avoid capturing lambdas in base::BindOnce
void PageMapExtractor::AfterResolverCollected(
    PageMapStreamCallback stream_cb) {
  if (stream_cb)
    stream_cb.Run(BuildPageMap());
  CompleteRun(BuildPageMap());
}

void PageMapExtractor::AfterDomCollected(
    content::WebContents* web_contents,
    bool greedy,
    PageMapStreamCallback stream_cb) {
  if (stream_cb)
    stream_cb.Run(BuildPageMap());
  RunStealthResolverAsync(
      this, web_contents, greedy,
      base::BindOnce(&PageMapExtractor::AfterResolverCollected,
                     weak_factory_.GetWeakPtr(), std::move(stream_cb)));
}

void PageMapExtractor::CompleteRun(base::Value::Dict page_map) {
  if (!run_in_progress_) {
    return;
  }
  run_in_progress_ = false;
  run_timeout_timer_.Stop();
  if (pending_run_callback_.is_null()) {
    return;
  }
  std::move(pending_run_callback_).Run(std::move(page_map));
}

void PageMapExtractor::OnRunTimeout() {
  if (!run_in_progress_) {
    return;
  }
  LOG(WARNING) << "[PageMapExtractor] Run timed out; returning partial page map";
  CompleteRun(BuildPageMap());
}

void PageMapExtractor::Run(content::WebContents* web_contents,
                           PageMapReadyCallback callback,
                           PageMapStreamCallback stream_cb,
                           bool greedy,
                           bool /*is_agent_mode*/,
                           int timeout_ms) {
  if (!web_contents) {
    std::move(callback).Run(base::Value::Dict());
    return;
  }

  run_timeout_timer_.Stop();
  run_in_progress_ = true;
  pending_run_callback_ = std::move(callback);
  if (timeout_ms > 0) {
    run_timeout_timer_.Start(
        FROM_HERE, base::Milliseconds(timeout_ms),
        base::BindOnce(&PageMapExtractor::OnRunTimeout,
                       weak_factory_.GetWeakPtr()));
  }

  auto after_dom = base::BindOnce(&PageMapExtractor::AfterDomCollected,
                                  weak_factory_.GetWeakPtr(), web_contents,
                                  greedy, std::move(stream_cb));
  RunDomCollectorAsync(this, web_contents, greedy, std::move(after_dom));
}

// Non-streaming convenience overload: forwards to the streaming variant
// with a null stream callback.
void PageMapExtractor::Run(content::WebContents* web_contents,
                           PageMapReadyCallback callback,
                           bool greedy,
                           bool is_agent_mode,
                           int timeout_ms) {
  Run(web_contents, std::move(callback), PageMapStreamCallback(), greedy,
      is_agent_mode, timeout_ms);
}

// Async placeholder signatures: if you already have async collectors, call them instead.
// They should invoke the provided callback when done (on the current sequence).
void RunDomCollectorAsync(PageMapExtractor* extractor,
                          content::WebContents* wc,
                          bool greedy,
                          base::OnceClosure done_cb) {
  // TODO: If you have an async DOM walker/injected helper, call it here and invoke done_cb when ready.
  // For now, run synchronous collector and immediately post the callback.
  CollectElementsSync(extractor, wc, greedy);
  std::move(done_cb).Run();
}

void RunStealthResolverAsync(PageMapExtractor* extractor,
                             content::WebContents* wc,
                             bool greedy,
                             base::OnceClosure done_cb) {
  if (!wc) {
    std::move(done_cb).Run();
    return;
  }

  content::RenderFrameHost* rfh = wc->GetPrimaryMainFrame();
  if (!rfh) {
    std::move(done_cb).Run();
    return;
  }

  auto resolver =
      std::make_unique<
          mojo::AssociatedRemote<dashboard::mojom::StealthyElementResolver>>();
  rfh->GetRemoteAssociatedInterfaces()->GetInterface(resolver.get());
  if (!resolver->is_bound()) {
    std::move(done_cb).Run();
    return;
  }

  auto weak_extractor = extractor->GetWeakPtr();
  (*resolver)->CollectAllRelevantElements(
      greedy ? dashboard::mojom::SearchScope::kAll
             : dashboard::mojom::SearchScope::kInteractiveOnly,
      base::BindOnce(
          [](base::WeakPtr<dashboard::PageMapExtractor> weak,
             base::OnceClosure done,
             std::unique_ptr<mojo::AssociatedRemote<
                 dashboard::mojom::StealthyElementResolver>> /*keep_alive*/,
             std::vector<dashboard::mojom::ResolvedElementInfoPtr> elements) {
            if (weak) {
              for (auto& info : elements) {
                weak->AddElement(ToSnapshot(std::move(info)));
              }
            }
            std::move(done).Run();
          },
          weak_extractor, std::move(done_cb), std::move(resolver)));
}

}  // namespace dashboard
