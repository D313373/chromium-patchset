#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"

#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/numerics/safe_conversions.h"
#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/glic/host/context/glic_page_context_fetcher.h"
#include "chrome/browser/glic/public/context/glic_sharing_manager.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/glic/host/glic.mojom.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "components/optimization_guide/content/browser/page_content_proto_util.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/mojom/content_extraction/ai_page_content.mojom.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect_f.h"

namespace dashboard {

namespace {

std::string NormalizeApcText(std::string text, size_t max_len = 160u) {
  text = base::CollapseWhitespaceASCII(text, true);
  if (text.size() <= max_len) {
    return text;
  }
  text.resize(max_len);
  text.append("...");
  return text;
}

std::string NormalizeUtf8BestEffort(std::string value) {
  if (value.empty() || base::IsStringUTF8AllowingNoncharacters(value)) {
    return value;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(value));
}

std::string ApcTagHintForAttributeType(
    optimization_guide::proto::ContentAttributeType type) {
  using T = optimization_guide::proto::ContentAttributeType;
  switch (type) {
    case T::CONTENT_ATTRIBUTE_ANCHOR:
      return "a";
    case T::CONTENT_ATTRIBUTE_FORM:
      return "form";
    case T::CONTENT_ATTRIBUTE_FORM_CONTROL:
      return "input";
    case T::CONTENT_ATTRIBUTE_TABLE:
      return "table";
    case T::CONTENT_ATTRIBUTE_TABLE_ROW:
      return "tr";
    case T::CONTENT_ATTRIBUTE_IMAGE:
      return "img";
    case T::CONTENT_ATTRIBUTE_VIDEO:
      return "video";
    case T::CONTENT_ATTRIBUTE_CANVAS:
      return "canvas";
    case T::CONTENT_ATTRIBUTE_SVG:
      return "svg";
    default:
      return std::string();
  }
}

void AppendUniqueLocator(
    base::Value::List& out,
    std::set<std::pair<std::string, std::string>>& seen,
    std::string type,
    std::string value) {
  value = NormalizeUtf8BestEffort(std::move(value));
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  if (type.empty() || value.empty()) {
    return;
  }
  if (!seen.insert({type, value}).second) {
    return;
  }
  base::Value::Dict locator;
  locator.Set("type", std::move(type));
  locator.Set("value", std::move(value));
  out.Append(std::move(locator));
}

struct SelectorSidecarEntry {
  int dom_node_id = 0;
  std::string document_identifier;
  std::string frame_id;
  std::string accessibility_name;
  std::string interactive_ancestor_role;
  std::string id_attr;
  std::string class_attr;
  std::string name_attr;
  std::string css_selector;
  std::string xpath;
  std::string shadow_chain_selector;
  std::string resource_path;
  bool in_shadow_tree = false;
};

std::string MakeSelectorKey(std::string_view document_identifier,
                            int dom_node_id) {
  return base::StringPrintf("%.*s#%d",
                            static_cast<int>(document_identifier.size()),
                            document_identifier.data(), dom_node_id);
}

base::Value::List BuildLocatorBundle(
    const optimization_guide::proto::ContentAttributes& attrs,
    const std::string& primary_text,
    const std::string& field_name,
    const std::string& placeholder,
    std::optional<gfx::Point> center,
    std::string_view document_identifier,
    const SelectorSidecarEntry* selector) {
  base::Value::List out;
  std::set<std::pair<std::string, std::string>> seen;

  if (selector) {
    AppendUniqueLocator(out, seen, "SHADOW_DOM", selector->shadow_chain_selector);
    AppendUniqueLocator(out, seen, "ACCESSIBILITY_NAME",
                        selector->accessibility_name);
    AppendUniqueLocator(out, seen, "ID", selector->id_attr);
    AppendUniqueLocator(out, seen, "CLASS_NAME", selector->class_attr);
    AppendUniqueLocator(out, seen, "NAME", selector->name_attr);
    AppendUniqueLocator(out, seen, "CSS_SELECTOR", selector->css_selector);
    AppendUniqueLocator(out, seen, "XPATH", selector->xpath);
  }

  if (!primary_text.empty()) {
    AppendUniqueLocator(out, seen, "ACCESSIBILITY_NAME", primary_text);
    AppendUniqueLocator(out, seen, "TEXT_CONTENT", primary_text);
  }
  if (!placeholder.empty()) {
    AppendUniqueLocator(out, seen, "TEXT_CONTENT", placeholder);
  }
  if (!field_name.empty()) {
    AppendUniqueLocator(out, seen, "NAME", field_name);
  }

  const std::string tag_hint = ApcTagHintForAttributeType(attrs.attribute_type());
  if (!tag_hint.empty()) {
    AppendUniqueLocator(out, seen, "TAG_NAME", tag_hint);
  }

  if (attrs.attribute_type() ==
          optimization_guide::proto::CONTENT_ATTRIBUTE_ANCHOR &&
      !primary_text.empty()) {
    AppendUniqueLocator(out, seen, "LINK_TEXT", primary_text);
    AppendUniqueLocator(out, seen, "PARTIAL_LINK_TEXT", primary_text);
  }

  if (center.has_value()) {
    AppendUniqueLocator(out, seen, "VISUAL_COORDS",
                        base::StringPrintf("{\"x\":%d,\"y\":%d}",
                                           center->x(), center->y()));
  }

  return out;
}

base::Value::Dict BuildMergedPageMap(
    const optimization_guide::proto::AnnotatedPageContent& apc,
    const GURL& fallback_url,
    const base::flat_map<std::string, SelectorSidecarEntry>& selector_entries) {
  dashboard::PageMapExtractor extractor;
  extractor.set_timestamp_ms(base::Time::Now().InMillisecondsSinceUnixEpoch());

  GURL page_url = fallback_url;
  std::string main_document_id;
  if (apc.has_main_frame_data()) {
    const auto& frame_data = apc.main_frame_data();
    if (!frame_data.url().empty()) {
      const GURL candidate(frame_data.url());
      if (candidate.is_valid()) {
        page_url = candidate;
      }
    }
    if (frame_data.has_document_identifier()) {
      main_document_id = frame_data.document_identifier().serialized_token();
    }
  }
  extractor.set_url(page_url);

  // APC element geometry is often device/screenshot pixels; viewport_geometry
  // is DIPs (same space as SetPositionInWidget). Scale centers into DIP space.
  float apc_to_dip = 1.0f;
  if (apc.has_viewport_geometry() && apc.has_root_node() &&
      apc.root_node().has_content_attributes() &&
      apc.root_node().content_attributes().has_geometry()) {
    const int vp_w = apc.viewport_geometry().width();
    const auto& root_geom = apc.root_node().content_attributes().geometry();
    const int root_w =
        root_geom.has_outer_bounding_box()
            ? root_geom.outer_bounding_box().width()
            : (root_geom.has_visible_bounding_box()
                   ? root_geom.visible_bounding_box().width()
                   : 0);
    if (vp_w > 0 && root_w > static_cast<int>(vp_w * 1.05f)) {
      const float scale = static_cast<float>(vp_w) / static_cast<float>(root_w);
      if (scale > 0.2f && scale <= 1.0f) {
        apc_to_dip = scale;
      }
    }
  }

  int visited_nodes = 0;
  int matched_selector_nodes = 0;
  optimization_guide::VisitContentNodes(
      apc.root_node(), main_document_id,
      [&](const optimization_guide::proto::ContentNode& node,
          std::string_view document_identifier) {
        ++visited_nodes;
        if (!node.has_content_attributes()) {
          return;
        }

        const auto& attrs = node.content_attributes();
        const int dom_node_id = attrs.common_ancestor_dom_node_id();
        if (dom_node_id <= 0) {
          return;
        }

        const SelectorSidecarEntry* selector = nullptr;
        auto selector_it =
            selector_entries.find(MakeSelectorKey(document_identifier, dom_node_id));
        if (selector_it != selector_entries.end()) {
          selector = &selector_it->second;
          ++matched_selector_nodes;
        }

        bool clickable = false;
        bool focusable = false;
        bool editable = false;
        bool disabled = false;
        if (attrs.has_interaction_info()) {
          const auto& interaction = attrs.interaction_info();
          clickable = interaction.is_clickable() ||
                      interaction.clickability_reasons_size() > 0;
          focusable = interaction.is_focusable();
          editable = interaction.is_editable();
          disabled = interaction.is_disabled();
        }

        std::string field_name;
        std::string placeholder;
        std::string type;
        std::string field_value;
        bool field_nonempty = false;
        if (attrs.has_form_control_data()) {
          const auto& form_control = attrs.form_control_data();
          field_name = NormalizeApcText(form_control.field_name(), 80u);
          placeholder = NormalizeApcText(form_control.placeholder(), 120u);
          type = optimization_guide::proto::FormControlType_Name(
              form_control.form_control_type());
          field_nonempty = !form_control.field_value().empty();
          // Password text stays in the renderer. The nonempty bit is enough
          // for the autofill latch.
          // FormControlType_Name is the enum token, e.g. FORM_CONTROL_TYPE_PASSWORD.
          if (field_nonempty && type.find("PASSWORD") == std::string::npos) {
            field_value = NormalizeApcText(form_control.field_value(), 200u);
          }
        }

        std::string primary_text = NormalizeApcText(attrs.label(), 160u);
        if (primary_text.empty() && attrs.has_text_data()) {
          primary_text = NormalizeApcText(attrs.text_data().text_content(), 160u);
        }
        if (primary_text.empty() && !placeholder.empty()) {
          primary_text = placeholder;
        } else if (primary_text.empty() && !field_name.empty()) {
          primary_text = field_name;
        }

        std::string role;
        if (attrs.has_aria_role()) {
          role = optimization_guide::proto::AXRole_Name(attrs.aria_role());
        }

        const bool actionable =
            clickable || focusable || editable || attrs.has_form_control_data();
        if (!actionable && primary_text.empty() && role.empty() && !selector) {
          return;
        }

        gfx::RectF bounds;
        std::optional<gfx::Point> center;
        if (attrs.has_geometry()) {
          const auto& geometry = attrs.geometry();
          const auto* box = geometry.has_visible_bounding_box()
                                ? &geometry.visible_bounding_box()
                                : (geometry.has_outer_bounding_box()
                                       ? &geometry.outer_bounding_box()
                                       : nullptr);
          if (box) {
            bounds = gfx::RectF(static_cast<float>(box->x()) * apc_to_dip,
                                static_cast<float>(box->y()) * apc_to_dip,
                                static_cast<float>(box->width()) * apc_to_dip,
                                static_cast<float>(box->height()) * apc_to_dip);
            if (box->width() > 0 && box->height() > 0) {
              center = gfx::Point(
                  base::ClampRound((box->x() + box->width() / 2) * apc_to_dip),
                  base::ClampRound((box->y() + box->height() / 2) * apc_to_dip));
            }
          }
        }

        dashboard::PageElementSnapshot snap;
        snap.element_id = base::NumberToString(dom_node_id);
        snap.frame_id =
            selector && !selector->frame_id.empty() ? selector->frame_id
                                                    : std::string();
        snap.tag = ApcTagHintForAttributeType(attrs.attribute_type());
        snap.role = role;
        snap.type = !type.empty()
                        ? type
                        : optimization_guide::proto::ContentAttributeType_Name(
                              attrs.attribute_type());
        snap.text = primary_text;
        snap.accessibility_name =
            selector && !selector->accessibility_name.empty()
                ? selector->accessibility_name
                : primary_text;
        snap.interactive_ancestor_role =
            selector ? selector->interactive_ancestor_role : std::string();
        snap.id_attr = selector ? selector->id_attr : std::string();
        snap.class_attr = selector ? selector->class_attr : std::string();
        snap.name_attr = selector && !selector->name_attr.empty()
                             ? selector->name_attr
                             : field_name;
        snap.placeholder = placeholder;
        snap.field_value = field_value;
        snap.field_nonempty = field_nonempty;
        snap.css_selector =
            selector ? selector->css_selector : std::string();
        snap.xpath = selector ? selector->xpath : std::string();
        snap.shadow_chain_selector =
            selector ? selector->shadow_chain_selector : std::string();
        snap.resource_path =
            selector ? selector->resource_path : std::string();
        snap.locator_bundle = BuildLocatorBundle(attrs, primary_text, field_name,
                                                placeholder, center,
                                                document_identifier, selector);
        snap.in_shadow_dom = selector && (selector->in_shadow_tree ||
                                          !selector->shadow_chain_selector.empty());
        snap.clickable = clickable;
        snap.focusable = focusable;
        snap.editable = editable;
        snap.disabled = disabled;
        snap.bounds = bounds;
        extractor.AddElement(std::move(snap));
      });

  base::Value::Dict page_map = extractor.BuildPageMap();
  if (base::Value::Dict* metadata = page_map.FindDict("metadata")) {
    metadata->Set("source", "glic_apc_selector_sidecar");
    metadata->Set("apcNodeCount", visited_nodes);
    metadata->Set("selectorSidecarCount",
                  static_cast<int>(selector_entries.size()));
    metadata->Set("selectorSidecarMatchedCount", matched_selector_nodes);
    if (!main_document_id.empty()) {
      metadata->Set("documentId", main_document_id);
    }
    if (apc.has_viewport_geometry()) {
      metadata->Set("viewportWidth", apc.viewport_geometry().width());
      metadata->Set("viewportHeight", apc.viewport_geometry().height());
    }
  }
  return page_map;
}

constexpr size_t kMaxAgentPageMapElements = 24;

bool IsInteractivePageMapElement(const base::Value::Dict& elem) {
  if (elem.FindBool("clickable").value_or(false)) {
    return true;
  }
  if (elem.FindBool("focusable").value_or(false)) {
    return true;
  }
  if (elem.FindBool("editable").value_or(false)) {
    return true;
  }
  const std::string* type = elem.FindString("type");
  if (!type) {
    return false;
  }
  return *type == "input" || *type == "button" || *type == "link" ||
         base::StartsWith(*type, "FORM_CONTROL_TYPE_",
                          base::CompareCase::INSENSITIVE_ASCII);
}

void StripHeavyDomLocatorsFromElement(base::Value::Dict& elem) {
  // Agent context should be accessibility-oriented. Drop CSS/XPath/shadow DOM
  // targeting bundles that bloat prompts and encourage DOM crawling.
  if (base::Value::Dict* selectors = elem.FindDict("selectors")) {
    selectors->Remove("css");
    selectors->Remove("xpath");
    selectors->Remove("shadowChain");
    if (base::Value::List* locators = selectors->FindList("locators")) {
      base::Value::List kept;
      for (auto& locator_value : *locators) {
        if (!locator_value.is_dict()) {
          continue;
        }
        const std::string* type = locator_value.GetDict().FindString("type");
        if (!type) {
          continue;
        }
        if (base::EqualsCaseInsensitiveASCII(*type, "CSS_SELECTOR") ||
            base::EqualsCaseInsensitiveASCII(*type, "XPATH") ||
            base::EqualsCaseInsensitiveASCII(*type, "SHADOW_DOM") ||
            base::EqualsCaseInsensitiveASCII(*type, "VISUAL_COORDS") ||
            base::EqualsCaseInsensitiveASCII(*type, "CLASS_NAME")) {
          continue;
        }
        kept.Append(std::move(locator_value));
      }
      selectors->Set("locators", std::move(kept));
    }
  }
  elem.Remove("css_selector");
  elem.Remove("xpath");
  elem.Remove("html");
  elem.Remove("outerHTML");
  elem.Remove("innerHTML");
}

std::string PageMapElementDedupeKey(const base::Value::Dict& elem) {
  if (const std::string* id = elem.FindString("id"); id && !id->empty()) {
    return "id:" + *id;
  }
  if (const base::Value::Dict* selectors = elem.FindDict("selectors")) {
    if (const std::string* css = selectors->FindString("css");
        css && !css->empty()) {
      return "css:" + *css;
    }
    if (const std::string* xpath = selectors->FindString("xpath");
        xpath && !xpath->empty()) {
      return "xpath:" + *xpath;
    }
  }
  if (const std::string* text = elem.FindString("text"); text && !text->empty()) {
    return "text:" + *text;
  }
  return std::string();
}

void FilterInteractiveElementsList(base::Value::List* elements,
                                 std::set<std::string>& seen_keys,
                                 size_t& kept_count) {
  if (!elements) {
    return;
  }
  base::Value::List filtered;
  for (auto& item : *elements) {
    if (kept_count >= kMaxAgentPageMapElements) {
      break;
    }
    if (!item.is_dict()) {
      continue;
    }
    const base::Value::Dict& elem = item.GetDict();
    if (!IsInteractivePageMapElement(elem)) {
      continue;
    }
    const std::string key = PageMapElementDedupeKey(elem);
    if (key.empty() || !seen_keys.insert(key).second) {
      continue;
    }
    ++kept_count;
    if (item.is_dict()) {
      StripHeavyDomLocatorsFromElement(item.GetDict());
    }
    filtered.Append(std::move(item));
  }
  *elements = std::move(filtered);
}

base::Value::Dict FilterPageMapForAgent(base::Value::Dict page_map) {
  base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return page_map;
  }

  std::set<std::string> seen_keys;
  size_t kept_count = 0;
  for (auto& frame_value : *frames) {
    if (!frame_value.is_dict()) {
      continue;
    }
    base::Value::Dict& frame = frame_value.GetDict();
    FilterInteractiveElementsList(frame.FindList("elements"), seen_keys,
                                  kept_count);
    if (kept_count >= kMaxAgentPageMapElements) {
      break;
    }
    base::Value::List* shadow_roots = frame.FindList("shadowRoots");
    if (!shadow_roots) {
      continue;
    }
    for (auto& shadow_value : *shadow_roots) {
      if (kept_count >= kMaxAgentPageMapElements) {
        break;
      }
      if (!shadow_value.is_dict()) {
        continue;
      }
      FilterInteractiveElementsList(shadow_value.GetDict().FindList("elements"),
                                    seen_keys, kept_count);
    }
  }

  if (base::Value::Dict* metadata = page_map.FindDict("metadata")) {
    metadata->Set("agentFilteredInteractiveCount",
                  static_cast<int>(kept_count));
    metadata->Set("agentFilterMaxElements",
                  static_cast<int>(kMaxAgentPageMapElements));
    metadata->Set("agentContextMode", "accessibility_trimmed");
  }
  return page_map;
}

glic::mojom::GetTabContextOptions BuildDashboardPageMapContextOptions() {
  glic::mojom::GetTabContextOptions options;
  options.include_viewport_screenshot = false;
  options.include_pdf = false;
  options.include_annotated_page_content = true;
  options.max_meta_tags = 0;
  options.annotated_page_content_mode =
      optimization_guide::proto::
          ANNOTATED_PAGE_CONTENT_MODE_ACTIONABLE_ELEMENTS;
  options.include_inner_text = false;
  options.inner_text_bytes_limit = 0u;
  return options;
}

std::vector<optimization_guide::AIPageContentSelectorEntry>
ConvertSelectorSidecar(
    const glic::mojom::AnnotatedPageDataPtr& annotated_page) {
  std::vector<optimization_guide::AIPageContentSelectorEntry> out;
  if (!annotated_page) {
    return out;
  }
  out.reserve(annotated_page->selector_sidecar.size());
  for (const auto& entry : annotated_page->selector_sidecar) {
    if (!entry) {
      continue;
    }
    optimization_guide::AIPageContentSelectorEntry selector;
    selector.document_identifier = entry->document_identifier;
    selector.frame_id = entry->frame_id;
    selector.dom_node_id = entry->dom_node_id;
    selector.id_attr = entry->id_attr;
    selector.class_attr = entry->class_attr;
    selector.name_attr = entry->name_attr;
    selector.css_selector = entry->css_selector;
    selector.xpath = entry->xpath;
    selector.shadow_chain_selector = entry->shadow_chain_selector;
    selector.resource_path = entry->resource_path;
    selector.accessibility_name = entry->accessibility_name;
    selector.interactive_ancestor_role = entry->interactive_ancestor_role;
    selector.in_shadow_tree = entry->in_shadow_tree;
    out.push_back(std::move(selector));
  }
  return out;
}

SelectorSidecarEntry BuildSelectorSidecarEntry(
    const optimization_guide::AIPageContentSelectorEntry& info) {
  SelectorSidecarEntry entry;
  entry.document_identifier = info.document_identifier;
  entry.dom_node_id = info.dom_node_id;
  entry.frame_id = info.frame_id;
  entry.accessibility_name = info.accessibility_name;
  entry.interactive_ancestor_role = info.interactive_ancestor_role;
  entry.id_attr = info.id_attr;
  entry.class_attr = info.class_attr;
  entry.name_attr = info.name_attr;
  entry.css_selector = info.css_selector;
  entry.xpath = info.xpath;
  entry.shadow_chain_selector = info.shadow_chain_selector;
  entry.resource_path = info.resource_path;
  entry.in_shadow_tree = info.in_shadow_tree;
  return entry;
}

class UnifiedPageMapFetcher : public base::RefCounted<UnifiedPageMapFetcher> {
 public:
  UnifiedPageMapFetcher(content::WebContents* web_contents,
                        UnifiedPageMapCallback callback,
                        mojom::SearchScope /*scope*/)
      : web_contents_(web_contents),
        callback_(std::move(callback)) {}

  void StartWithFetch() {
    TRACE_EVENT0("loading", "UnifiedPageMapFetcher::StartWithFetch");
    if (!web_contents_) {
      FinishWithError("no_web_contents");
      return;
    }
    fallback_url_ = web_contents_->GetLastCommittedURL();
    tabs::TabInterface* tab =
        tabs::TabInterface::GetFromContents(web_contents_.get());
    if (!tab) {
      FinishWithError("no_tab_for_apc");
      return;
    }
    glic::GlicKeyedService* service =
        glic::GlicKeyedServiceFactory::GetGlicKeyedService(
            web_contents_->GetBrowserContext(), /*create=*/false);
    const glic::mojom::GetTabContextOptions options =
        BuildDashboardPageMapContextOptions();
    if (service) {
      // Dashboard automation often targets background or non-focused tabs.
      // Stay on the Chromium/Glic-native context pipeline, but use the actor
      // variant so automation is not rejected by focused/pinned sharing gates.
      service->sharing_manager().GetContextForActorFromTab(
          tab->GetHandle(), options,
          base::BindOnce(&UnifiedPageMapFetcher::OnGlicContextFetched,
                         base::RetainedRef(this)));
      return;
    }
    // Headless listener skips Glic startup. Annotated Page Content itself does
    // not need the panel or the keyed service.
    glic::FetchPageContext(
        tab, options,
        base::BindOnce(&UnifiedPageMapFetcher::OnDirectPageContextFetched,
                       base::RetainedRef(this)));
  }

  void StartWithAnnotatedPageContent(
      optimization_guide::proto::AnnotatedPageContent annotated_page_content,
      const GURL& fallback_url,
      const std::vector<optimization_guide::AIPageContentSelectorEntry>&
          selector_sidecar) {
    apc_ = std::move(annotated_page_content);
    fallback_url_ = fallback_url;
    StoreSelectorSidecar(selector_sidecar);
    FinishSuccess();
  }

 private:
  friend class base::RefCounted<UnifiedPageMapFetcher>;

  ~UnifiedPageMapFetcher() = default;

  void OnDirectPageContextFetched(
      base::expected<glic::mojom::GetContextResultPtr,
                     page_content_annotations::FetchPageContextErrorDetails>
          result) {
    TRACE_EVENT1("loading", "UnifiedPageMapFetcher::OnDirectPageContextFetched",
                 "ok", result.has_value());
    if (!result.has_value()) {
      FinishWithError(std::string("apc_context_error:") +
                      result.error().message);
      return;
    }
    ConsumeContextResult(std::move(result.value()));
  }

  void OnGlicContextFetched(glic::GlicGetContextResult result) {
    TRACE_EVENT1("loading", "UnifiedPageMapFetcher::OnGlicContextFetched", "ok",
                 result.has_value());
    if (!result.has_value()) {
      FinishWithError(std::string("glic_context_error:") + result.error().message);
      return;
    }
    ConsumeContextResult(std::move(result.value()));
  }

  void ConsumeContextResult(glic::mojom::GetContextResultPtr result) {
    if (!result || !result->is_tab_context()) {
      FinishWithError(result && result->is_error_reason()
                          ? result->get_error_reason()
                          : "glic_context_not_tab");
      return;
    }

    const glic::mojom::TabContextPtr& tab_context = result->get_tab_context();
    if (tab_context->tab_data && tab_context->tab_data->url.is_valid()) {
      fallback_url_ = tab_context->tab_data->url;
    }
    if (!tab_context->annotated_page_data ||
        !tab_context->annotated_page_data->annotated_page_content) {
      FinishWithError("glic_apc_empty");
      return;
    }
    std::optional<optimization_guide::proto::AnnotatedPageContent> apc =
        tab_context->annotated_page_data->annotated_page_content
            ->As<optimization_guide::proto::AnnotatedPageContent>();
    if (!apc.has_value() || !apc->has_root_node()) {
      FinishWithError("glic_apc_parse_failed");
      return;
    }
    apc_ = std::move(*apc);
    StoreSelectorSidecar(
        ConvertSelectorSidecar(tab_context->annotated_page_data));
    FinishSuccess();
  }

  void StoreSelectorSidecar(
      const std::vector<optimization_guide::AIPageContentSelectorEntry>&
          selector_sidecar) {
    selector_entries_.clear();
    for (const auto& entry : selector_sidecar) {
      if (entry.dom_node_id <= 0 || entry.document_identifier.empty()) {
        continue;
      }
      selector_entries_.insert_or_assign(
          MakeSelectorKey(entry.document_identifier, entry.dom_node_id),
          BuildSelectorSidecarEntry(entry));
    }
  }

  void FinishSuccess() {
    TRACE_EVENT0("loading", "UnifiedPageMapFetcher::FinishSuccess");
    UnifiedPageMapResult result;
    result.ok = true;
    result.page_map = FilterPageMapForAgent(
        BuildMergedPageMap(apc_, fallback_url_, selector_entries_));
    std::move(callback_).Run(std::move(result));
  }

  void FinishWithError(std::string error) {
    TRACE_EVENT1("loading", "UnifiedPageMapFetcher::FinishWithError",
                 "error", error);
    UnifiedPageMapResult result;
    result.ok = false;
    result.error = std::move(error);
    std::move(callback_).Run(std::move(result));
  }

  raw_ptr<content::WebContents> web_contents_ = nullptr;
  UnifiedPageMapCallback callback_;
  GURL fallback_url_;
  optimization_guide::proto::AnnotatedPageContent apc_;
  base::flat_map<std::string, SelectorSidecarEntry> selector_entries_;
};

UnifiedPageMapResult CloneUnifiedPageMapResult(
    const UnifiedPageMapResult& result) {
  UnifiedPageMapResult clone;
  clone.ok = result.ok;
  clone.error = result.error;
  clone.page_map = result.page_map.Clone();
  return clone;
}

UnifiedPageMapResult BuildUnifiedPageMapErrorResult(std::string error) {
  UnifiedPageMapResult result;
  result.ok = false;
  result.error = std::move(error);
  return result;
}

class UnifiedPageMapRequestCoordinator : public base::SupportsUserData::Data,
                                         public content::WebContentsObserver {
 public:
  static UnifiedPageMapRequestCoordinator* GetOrCreate(
      content::WebContents* web_contents) {
    CHECK(web_contents);
    auto* coordinator = static_cast<UnifiedPageMapRequestCoordinator*>(
        web_contents->GetUserData(UserDataKey()));
    if (coordinator) {
      return coordinator;
    }

    auto owned = std::unique_ptr<UnifiedPageMapRequestCoordinator>(
        new UnifiedPageMapRequestCoordinator(web_contents));
    coordinator = owned.get();
    web_contents->SetUserData(UserDataKey(), std::move(owned));
    return coordinator;
  }

  void Request(UnifiedPageMapCallback callback, mojom::SearchScope scope) {
    TRACE_EVENT2("loading", "UnifiedPageMapRequestCoordinator::Request",
                 "pending_callbacks",
                 static_cast<int>(pending_callbacks_.size()), "fetch_in_progress",
                 fetch_in_progress_);
    if (!web_contents()) {
      std::move(callback).Run(BuildUnifiedPageMapErrorResult("no_web_contents"));
      return;
    }

    pending_callbacks_.push_back(std::move(callback));
    last_requested_scope_ = scope;
    if (fetch_in_progress_) {
      return;
    }
    StartFetch();
  }

  void InvalidateCache() { ++generation_; }

  void DidFinishNavigation(content::NavigationHandle* navigation_handle) override {
    if (navigation_handle && navigation_handle->IsInPrimaryMainFrame() &&
        navigation_handle->HasCommitted()) {
      ++generation_;
    }
  }

  void WebContentsDestroyed() override {
    FailPendingCallbacks("web_contents_destroyed");
    Observe(nullptr);
  }

 private:
  explicit UnifiedPageMapRequestCoordinator(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents) {}

  static const void* UserDataKey() {
    static const int kKey = 0;
    return &kKey;
  }

  void StartFetch() {
    TRACE_EVENT0("loading", "UnifiedPageMapRequestCoordinator::StartFetch");
    content::WebContents* current_web_contents = web_contents();
    if (!current_web_contents) {
      FailPendingCallbacks("no_web_contents");
      return;
    }

    fetch_in_progress_ = true;
    in_flight_generation_ = generation_;
    in_flight_url_ = current_web_contents->GetLastCommittedURL();

    auto fetcher = base::MakeRefCounted<UnifiedPageMapFetcher>(
        current_web_contents,
        base::BindOnce(&UnifiedPageMapRequestCoordinator::OnFetchComplete,
                       weak_ptr_factory_.GetWeakPtr(), in_flight_generation_,
                       in_flight_url_),
        last_requested_scope_);
    fetcher->StartWithFetch();
  }

  void OnFetchComplete(uint64_t fetch_generation,
                       GURL fetch_url,
                       UnifiedPageMapResult result) {
    TRACE_EVENT2("loading", "UnifiedPageMapRequestCoordinator::OnFetchComplete",
                 "ok", result.ok, "generation_match",
                 fetch_generation == generation_);
    fetch_in_progress_ = false;

    content::WebContents* current_web_contents = web_contents();
    if (!current_web_contents) {
      FailPendingCallbacks("no_web_contents");
      return;
    }

    if (fetch_generation != generation_ ||
        current_web_contents->GetLastCommittedURL() != fetch_url) {
      if (!pending_callbacks_.empty()) {
        StartFetch();
      }
      return;
    }

    DispatchPendingCallbacks(std::move(result));
  }

  void FailPendingCallbacks(std::string error) {
    fetch_in_progress_ = false;
    DispatchPendingCallbacks(BuildUnifiedPageMapErrorResult(std::move(error)));
  }

  void DispatchPendingCallbacks(UnifiedPageMapResult result) {
    std::vector<UnifiedPageMapCallback> callbacks;
    callbacks.swap(pending_callbacks_);
    if (callbacks.empty()) {
      return;
    }

    for (size_t i = 0; i + 1 < callbacks.size(); ++i) {
      std::move(callbacks[i]).Run(CloneUnifiedPageMapResult(result));
    }
    std::move(callbacks.back()).Run(std::move(result));
  }

  std::vector<UnifiedPageMapCallback> pending_callbacks_;
  mojom::SearchScope last_requested_scope_ = mojom::SearchScope::kInteractiveOnly;
  uint64_t generation_ = 0;
  uint64_t in_flight_generation_ = 0;
  bool fetch_in_progress_ = false;
  GURL in_flight_url_;
  base::WeakPtrFactory<UnifiedPageMapRequestCoordinator> weak_ptr_factory_{
      this};
};

}  // namespace

UnifiedPageMapResult::UnifiedPageMapResult() = default;
UnifiedPageMapResult::UnifiedPageMapResult(UnifiedPageMapResult&&) noexcept =
    default;
UnifiedPageMapResult& UnifiedPageMapResult::operator=(
    UnifiedPageMapResult&&) noexcept = default;
UnifiedPageMapResult::~UnifiedPageMapResult() = default;

void FetchAgentPageMap(content::WebContents* web_contents,
                       UnifiedPageMapCallback callback) {
  if (!web_contents) {
    std::move(callback).Run(BuildUnifiedPageMapErrorResult("no_web_contents"));
    return;
  }
  UnifiedPageMapRequestCoordinator::GetOrCreate(web_contents)->Request(
      std::move(callback), mojom::SearchScope::kInteractiveOnly);
}

void FetchUnifiedPageMapSnapshot(content::WebContents* web_contents,
                                 UnifiedPageMapCallback callback,
                                 mojom::SearchScope scope) {
  if (!ExecutionRunContext::Get().ShouldUseUnifiedPageMap()) {
    std::move(callback).Run(
        BuildUnifiedPageMapErrorResult("page_map_disabled_in_script_mode"));
    return;
  }
  if (!web_contents) {
    std::move(callback).Run(BuildUnifiedPageMapErrorResult("no_web_contents"));
    return;
  }
  UnifiedPageMapRequestCoordinator::GetOrCreate(web_contents)->Request(
      std::move(callback), scope);
}

void InvalidateUnifiedPageMapCache(content::WebContents* web_contents) {
  if (!web_contents) {
    return;
  }
  UnifiedPageMapRequestCoordinator::GetOrCreate(web_contents)->InvalidateCache();
}

void BuildUnifiedPageMapFromAnnotatedPageContent(
    content::WebContents* web_contents,
    optimization_guide::proto::AnnotatedPageContent annotated_page_content,
    const GURL& fallback_url,
    const std::vector<optimization_guide::AIPageContentSelectorEntry>&
        selector_sidecar,
    UnifiedPageMapCallback callback,
    mojom::SearchScope scope) {
  auto fetcher = base::MakeRefCounted<UnifiedPageMapFetcher>(
      web_contents, std::move(callback), scope);
  fetcher->StartWithAnnotatedPageContent(std::move(annotated_page_content),
                                         fallback_url, selector_sidecar);
}

void BuildUnifiedPageMapFromAnnotatedPageContent(
    content::WebContents* web_contents,
    optimization_guide::proto::AnnotatedPageContent annotated_page_content,
    const GURL& fallback_url,
    UnifiedPageMapCallback callback,
    mojom::SearchScope scope) {
  static const std::vector<optimization_guide::AIPageContentSelectorEntry>
      kEmptySelectorSidecar;
  auto fetcher = base::MakeRefCounted<UnifiedPageMapFetcher>(
      web_contents, std::move(callback), scope);
  fetcher->StartWithAnnotatedPageContent(std::move(annotated_page_content),
                                         fallback_url, kEmptySelectorSidecar);
}

}  // namespace dashboard
