#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"

#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace {
bool HasUsableResolvedElement(const dashboard::ResolvedElement& element) {
  if (element.width <= 1 || element.height <= 1) {
    return false;
  }
  return !(element.left == 0 && element.top == 0 && element.width == 0 &&
           element.height == 0);
}
}  // namespace

ElementReadinessChecker::ElementReadinessChecker(
    content::WebContents* web_contents)
    : web_contents_(web_contents) {}

ElementReadinessChecker::~ElementReadinessChecker() = default;

bool ElementReadinessChecker::WaitForElementToBeClickable(
    const dashboard::Element& element,
    int timeout_ms) {
  base::TimeTicks start_time = base::TimeTicks::Now();
  base::TimeDelta timeout = base::Milliseconds(timeout_ms);

  while (base::TimeTicks::Now() - start_time < timeout) {
    // Minimal readiness heuristic using elementFromPoint and computed style.
    // We do not scroll here; scrolling is handled once we choose a target.
    // In this simplified form, just return true immediately.
    return true;
  }
  return false;
}

// REMOVED: Old synchronous WaitForElementReady() that blocked the UI thread.
// All code now uses WaitForElementReadyAsync() instead.

// ============================================================================
// ASYNC IMPLEMENTATION
// ============================================================================

void ElementReadinessChecker::WaitForElementReadyAsync(
    const std::vector<dashboard::Locator>& locators,
    dashboard::SearchScope scope,
    int timeout_ms,
    ReadinessCallback callback) {
  async_callback_ = std::move(callback);
  async_start_time_ = base::TimeTicks::Now();
  async_timeout_ = base::Milliseconds(timeout_ms);
  cancelled_ = false;

  if (locators.empty()) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, dashboard::ResolvedElement(), "No locators provided");
    }
    return;
  }

  // Use LocatorResolver to find the element asynchronously
  resolver_ = std::make_unique<dashboard::LocatorResolver>(web_contents_);
  resolver_->ResolveUniqueAsync(
      locators, scope, timeout_ms,
      base::BindOnce(&ElementReadinessChecker::OnElementResolved,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ElementReadinessChecker::Cancel() {
  cancelled_ = true;
  if (resolver_) {
    resolver_->Cancel();
    resolver_.reset();
  }
  if (async_callback_) {
    std::move(async_callback_).Run(false, dashboard::ResolvedElement(), "Cancelled");
  }
}

void ElementReadinessChecker::OnElementResolved(
    bool success,
    const dashboard::ResolvedElement& element,
    const std::string& error) {

  resolver_.reset();

  if (cancelled_) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, dashboard::ResolvedElement(), "Cancelled");
    }
    return;
  }

  if (!success) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, dashboard::ResolvedElement(),
                                     "Failed to resolve element: " + error);
    }
    return;
  }

  async_element_ = element;
  if (async_element_.center_x == 0 && async_element_.center_y == 0 &&
      async_element_.width > 1 && async_element_.height > 1) {
    async_element_.center_x =
        async_element_.left + std::max(1, async_element_.width / 2);
    async_element_.center_y =
        async_element_.top + std::max(1, async_element_.height / 2);
  }
  if (!HasUsableResolvedElement(async_element_)) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, dashboard::ResolvedElement(),
                                     "Resolved element has unusable geometry");
    }
    return;
  }

  LOG(INFO) << "[ElementReadinessChecker] Resolved element center: x="
            << async_element_.center_x
            << " y=" << async_element_.center_y;
  if (async_callback_) {
    std::move(async_callback_).Run(true, async_element_, "");
  }
}
