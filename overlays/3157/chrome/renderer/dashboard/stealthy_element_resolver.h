// Copyright 2025 The Chromium Authors
// Minimal scaffolding for a renderer-side Blink resolver.

#ifndef CHROME_RENDERER_DASHBOARD_STEALTHY_ELEMENT_RESOLVER_H_
#define CHROME_RENDERER_DASHBOARD_STEALTHY_ELEMENT_RESOLVER_H_

#include <map>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"

namespace dashboard_renderer {

// Binds per RenderFrame and services ElementResolver + StealthyElementResolver
// mojo calls using Blink APIs.
class StealthyElementResolver : public content::RenderFrameObserver,
                                public dashboard::mojom::ElementResolver,
                                public dashboard::mojom::StealthyElementResolver {
 public:
  explicit StealthyElementResolver(content::RenderFrame* render_frame);

  ~StealthyElementResolver() override;

  void OnDestruct() override;

  // dashboard::mojom::ElementResolver
  void ResolveElementStealthily(
      std::vector<dashboard::mojom::LocatorPtr> locators,
      dashboard::mojom::SearchScope scope,
      ResolveElementStealthilyCallback callback) override;
  void GetElementFeatures(int32_t node_id, GetElementFeaturesCallback callback) override;
  void ResolveAtPoint(int32_t x, int32_t y, ResolveAtPointCallback callback) override;

  // dashboard::mojom::StealthyElementResolver
  void CollectAllRelevantElements(
      dashboard::mojom::SearchScope scope,
      CollectAllRelevantElementsCallback callback) override;
  void ResolveAllMatches(std::vector<dashboard::mojom::LocatorPtr> locators,
                         dashboard::mojom::SearchScope scope,
                         uint32_t max_results,
                         ResolveAllMatchesCallback callback) override;
  void FindRepeatedPeersAtPoint(
      int32_t x,
      int32_t y,
      uint32_t max_peers,
      FindRepeatedPeersAtPointCallback callback) override;
  void InvestigatePage(uint32_t max_links,
                       uint32_t max_inputs,
                       uint32_t max_buttons,
                       uint32_t max_text_chars,
                       InvestigatePageCallback callback) override;

  void BindElementResolver(
      mojo::PendingAssociatedReceiver<dashboard::mojom::ElementResolver> receiver);
  void BindStealthyResolver(
      mojo::PendingAssociatedReceiver<dashboard::mojom::StealthyElementResolver> receiver);

 private:
  dashboard::mojom::ResolvedElementInfoPtr BuildResolvedElement(
      blink::WebElement& element);

  // Helper to find an element via XPath.
  blink::WebElement FindByXPath(blink::WebDocument& document,
                                const std::string& xpath_query);

  raw_ptr<content::RenderFrame> render_frame_ = nullptr;
  mojo::AssociatedReceiverSet<dashboard::mojom::ElementResolver> element_receivers_;
  mojo::AssociatedReceiverSet<dashboard::mojom::StealthyElementResolver> stealth_receivers_;

  // Cache to hold found elements, keyed by their renderer-side Node ID.
  // This allows GetElementFeatures to retrieve them after resolution.
  std::map<int32_t, blink::WebElement> element_cache_;
  int32_t next_node_id_ = 0;

  base::WeakPtrFactory<StealthyElementResolver> weak_factory_{this};
};

}  // namespace dashboard_renderer

#endif  // CHROME_RENDERER_DASHBOARD_STEALTHY_ELEMENT_RESOLVER_H_
