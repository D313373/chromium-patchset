// Copyright ...
#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_PAGE_MAP_EXTRACTOR_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_PAGE_MAP_EXTRACTOR_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "url/gurl.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/rect_f.h"

namespace dashboard {

// Lightweight snapshot of a single interactive element on the page.
struct PageElementSnapshot {
  // Explicit out-of-line constructor/destructor to satisfy style.
  PageElementSnapshot();
  PageElementSnapshot(const PageElementSnapshot&) = delete;
  PageElementSnapshot(PageElementSnapshot&&) noexcept;
  PageElementSnapshot& operator=(const PageElementSnapshot&) = delete;
  PageElementSnapshot& operator=(PageElementSnapshot&&) noexcept;
  ~PageElementSnapshot();

  // Optional stable id from your resolver (hash, node id, etc.).
  std::string element_id;

  // DevTools / frame tree id for the frame this element lives in.
  std::string frame_id;

  // HTML tag, e.g. "button", "input".
  std::string tag;

  // Optional accessibility role + name.
  std::string role;
  std::string accessibility_name;
  std::string interactive_ancestor_role;
  std::string id_attr;
  std::string class_attr;
  std::string name_attr;

  // High-level semantic type for the agent ("clickable", "input", "link", etc.).
  std::string type;

  // Visible text / label (truncated by caller if desired).
  std::string text;

  // Selector bundle from StealthyLocatorResolver.
  std::string css_selector;
  std::string xpath;
  std::string shadow_chain_selector;  // e.g. "my-host >> div.btn"
  std::string resource_path;          // e.g. "/frames/0/shadow:host>>inner"
  // Optional richer locator bundle for downstream consumers.
  base::Value::List locator_bundle;

  // Shadow DOM / frame placement.
  bool in_shadow_dom = false;
  std::string shadow_host_selector;   // CSS selector for host, if any.

  bool clickable = false;
  bool focusable = false;
  bool editable = false;
  bool disabled = false;

  // From APC FormControlData. Password text is never stored.
  std::string placeholder;
  std::string field_value;
  bool field_nonempty = false;

  // Viewport coordinates in CSS pixels.
  gfx::RectF bounds;

  // Serialize this element into the pagemap JSON format.
  base::Value::Dict ToValueDict() const;
};

// Aggregates PageElementSnapshot objects into an AI-friendly "pagemap".
class PageMapExtractor {
 public:
  PageMapExtractor();
  ~PageMapExtractor();

  PageMapExtractor(const PageMapExtractor&) = delete;
  PageMapExtractor& operator=(const PageMapExtractor&) = delete;

  // Basic metadata.
  void set_url(const GURL& url) { url_ = url; }
  void set_timestamp_ms(int64_t timestamp_ms) { timestamp_ms_ = timestamp_ms; }

  // Optional currently focused element id.
  void set_focused_element_id(const std::string& id) {
    focused_element_id_ = id;
  }
  base::WeakPtr<PageMapExtractor> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  using PageMapReadyCallback = base::OnceCallback<void(base::Value::Dict)>;
  using PageMapStreamCallback = base::RepeatingCallback<void(const base::Value::Dict&)>;
  void Run(content::WebContents* web_contents,
           PageMapReadyCallback callback,
           bool greedy = false,
           bool is_agent_mode = false,
           int timeout_ms = 2500);

  // Streaming overload: emits intermediate snapshots via stream_cb after each
  // async collector finishes, and finally invokes callback with the final map.
  void Run(content::WebContents* web_contents,
           PageMapReadyCallback callback,
           PageMapStreamCallback stream_cb,
           bool greedy = false,
           bool is_agent_mode = false,
           int timeout_ms = 2500);

  // Add a single element snapshot (usually from StealthyLocatorResolver /
  // DevTools snapshot).
  void AddElement(PageElementSnapshot element);

  // Build a normalized pagemap JSON:
  //
  // {
  //   "url": "...",
  //   "timestamp": 123456,
  //   "frames": [ ... ],
  //   "metadata": { ... }
  // }
  //
  base::Value::Dict BuildPageMap() const;

 private:
  void AfterDomCollected(content::WebContents* web_contents,
                         bool greedy,
                         PageMapStreamCallback stream_cb);
  void AfterResolverCollected(PageMapStreamCallback stream_cb);
  void CompleteRun(base::Value::Dict page_map);
  void OnRunTimeout();

  bool run_in_progress_ = false;
  base::OneShotTimer run_timeout_timer_;
  PageMapReadyCallback pending_run_callback_;

  struct ShadowBucket {
    std::string host_selector;
    base::Value::List elements;  // of PageElementSnapshot::ToValueDict()
  };

  struct FrameBucket {
    FrameBucket();
    FrameBucket(const FrameBucket&) = delete;
    FrameBucket(FrameBucket&&) noexcept;
    FrameBucket& operator=(const FrameBucket&) = delete;
    FrameBucket& operator=(FrameBucket&&) noexcept;
    ~FrameBucket();

    std::string frame_id;
    base::Value::List elements;  // top-level frame elements
    std::map<std::string, ShadowBucket> shadow_buckets;  // key = host selector
  };

  GURL url_;
  int64_t timestamp_ms_ = 0;

  std::vector<PageElementSnapshot> elements_;
  std::string focused_element_id_;

  base::WeakPtrFactory<PageMapExtractor> weak_factory_{this};
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_PAGE_MAP_EXTRACTOR_H_
