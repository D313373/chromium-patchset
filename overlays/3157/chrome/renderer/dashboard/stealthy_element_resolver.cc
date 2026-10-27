// Copyright 2025

#include "chrome/renderer/dashboard/stealthy_element_resolver.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_element_collection.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_frame_content_dumper.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_hit_test_result.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/web/web_ax_object.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"

namespace dashboard_renderer {

namespace {

struct AXSnapshot {
  blink::WebAXObject ax;
  bool valid = false;
  bool ignored = false;
  bool clickable = false;
  bool focusable = false;
  bool disabled = false;
  std::string name;
  std::string role;
};

AXSnapshot GetAXSnapshot(const blink::WebElement& el) {
  AXSnapshot out;
  if (el.IsNull())
    return out;
  out.ax = blink::WebAXObject::FromWebNode(el);
  if (out.ax.IsNull() || out.ax.IsDetached())
    return out;
  out.valid = true;
  out.ignored = out.ax.IsIgnored();
  if (out.ignored || !out.ax.IsIncludedInTree())
    return out;
  out.clickable = out.ax.IsClickable();

  ui::AXNodeData data;
  ui::AXMode mode(ui::AXMode::kWebContents | ui::AXMode::kInlineTextBoxes |
                  ui::AXMode::kExtendedProperties);
  out.ax.Serialize(&data, mode);
  out.disabled =
      data.GetRestriction() == ax::mojom::Restriction::kDisabled;
  out.focusable = data.HasState(ax::mojom::State::kFocusable);
  out.role = ui::ToString(data.role);

  ax::mojom::NameFrom name_from = ax::mojom::NameFrom::kNone;
  std::vector<blink::WebAXObject> name_objects;
  out.name = out.ax.GetName(name_from, name_objects).Utf8();
  return out;
}

std::string LowerTagName(const blink::WebElement& element) {
  return base::ToLowerASCII(element.TagName().Utf8());
}

std::string EscapeCssAttributeValue(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::optional<std::pair<std::string, std::string>> PreferredStableAttr(
    const blink::WebElement& element) {
  static constexpr const char* kPreferredDataAttrs[] = {
      "data-testid",
      "data-test",
      "data-cy",
      "data-qa",
  };
  for (const char* attr_name : kPreferredDataAttrs) {
    const std::string value =
        element.GetAttribute(blink::WebString::FromUTF8(attr_name)).Utf8();
    if (!value.empty()) {
      return std::make_pair(std::string(attr_name), value);
    }
  }
  return std::nullopt;
}

std::string BuildCssSelectorForAttr(std::string_view attr_name,
                                    std::string_view value) {
  if (attr_name.empty() || value.empty()) {
    return std::string();
  }
  return "[" + std::string(attr_name) + "=\"" +
         EscapeCssAttributeValue(std::string(value)) + "\"]";
}

std::string BuildXPathForAttr(std::string_view attr_name,
                              std::string_view value) {
  if (attr_name.empty() || value.empty() ||
      std::string(value).find('\'') != std::string::npos) {
    return std::string();
  }
  return "//*[@" + std::string(attr_name) + "='" + std::string(value) + "']";
}

blink::WebElement ParentOrShadowHostElement(const blink::WebElement& element) {
  if (element.IsNull()) {
    return blink::WebElement();
  }
  blink::WebNode parent = element.ParentOrShadowHostNode();
  if (parent.IsNull() || !parent.IsElementNode()) {
    return blink::WebElement();
  }
  return parent.To<blink::WebElement>();
}

bool IsLikelyGeneratedIdentifierToken(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  const std::string lower = base::ToLowerASCII(token);
  if (base::StartsWith(lower, "css-") || base::StartsWith(lower, "jsx-") ||
      base::StartsWith(lower, "sc-") || base::StartsWith(lower, "r-") ||
      base::StartsWith(lower, "id__") || base::StartsWith(lower, "_")) {
    return true;
  }
  int digits = 0;
  int letters = 0;
  int hyphens = 0;
  for (char c : token) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc)) {
      ++digits;
    } else if (std::isalpha(uc)) {
      ++letters;
    } else if (c == '-' || c == '_') {
      ++hyphens;
    }
  }
  if (token.size() >= 8 && digits >= 3) {
    return true;
  }
  return token.size() >= 10 && digits >= 2 && letters >= 4 && hyphens >= 1;
}

bool IsVolatileClassNameList(std::string_view class_attr) {
  std::vector<std::string> tokens = base::SplitString(
      std::string(class_attr), " \t\r\n.", base::KEEP_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);
  if (tokens.empty()) {
    return false;
  }
  int volatile_tokens = 0;
  for (const std::string& token : tokens) {
    if (IsLikelyGeneratedIdentifierToken(token)) {
      ++volatile_tokens;
    }
  }
  if (volatile_tokens == 0) {
    return false;
  }
  if (tokens.size() == 1) {
    return true;
  }
  if (tokens.size() >= 4 && volatile_tokens * 2 >= static_cast<int>(tokens.size())) {
    return true;
  }
  return volatile_tokens >= 3;
}

std::string BuildClassSelector(std::string_view class_attr) {
  std::string css = ".";
  for (char c : class_attr) {
    css.push_back(c == ' ' ? '.' : c);
  }
  return css;
}

struct PreferredSelectorChoice {
  std::string css;
  std::string xpath;
};

PreferredSelectorChoice ChoosePreferredSelector(const blink::WebElement& element,
                                                std::string_view id_attr,
                                                std::string_view class_attr) {
  if (std::optional<std::pair<std::string, std::string>> self_attr =
          PreferredStableAttr(element)) {
    return {BuildCssSelectorForAttr(self_attr->first, self_attr->second),
            BuildXPathForAttr(self_attr->first, self_attr->second)};
  }

  if (!id_attr.empty() && !IsLikelyGeneratedIdentifierToken(id_attr)) {
    return {"#" + std::string(id_attr),
            "//*[@id='" + std::string(id_attr) + "']"};
  }

  blink::WebElement current = ParentOrShadowHostElement(element);
  for (int depth = 0; !current.IsNull() && depth < 6; ++depth) {
    if (std::optional<std::pair<std::string, std::string>> ancestor_attr =
            PreferredStableAttr(current)) {
      return {BuildCssSelectorForAttr(ancestor_attr->first, ancestor_attr->second),
              BuildXPathForAttr(ancestor_attr->first, ancestor_attr->second)};
    }
    current = ParentOrShadowHostElement(current);
  }

  if (!id_attr.empty()) {
    return {"#" + std::string(id_attr),
            "//*[@id='" + std::string(id_attr) + "']"};
  }

  if (!class_attr.empty() && !IsVolatileClassNameList(class_attr)) {
    return {BuildClassSelector(class_attr), std::string()};
  }

  const std::string tag_lower = LowerTagName(element);
  return {tag_lower, "//" + tag_lower};
}

std::vector<std::string> CollectPromotedStableAncestorCssSelectors(
    const blink::WebElement& element) {
  std::vector<std::string> selectors;
  std::set<std::string> seen;
  blink::WebElement current = element;
  for (int depth = 0; !current.IsNull() && depth < 6; ++depth) {
    if (std::optional<std::pair<std::string, std::string>> attr =
            PreferredStableAttr(current)) {
      const std::string css =
          BuildCssSelectorForAttr(attr->first, attr->second);
      if (!css.empty() && seen.insert(css).second) {
        selectors.push_back(css);
      }
    }
    current = ParentOrShadowHostElement(current);
  }
  return selectors;
}

std::string BuildShadowChainSelectorCore(const blink::WebElement& element) {
  std::vector<std::string> segments;
  blink::WebElement current = element;
  while (!current.IsNull()) {
    blink::WebElement host = current.OwnerShadowHost();
    if (host.IsNull()) {
      break;
    }
    segments.push_back(LowerTagName(host));
    current = host;
  }
  if (segments.empty())
    return std::string();
  std::reverse(segments.begin(), segments.end());
  return base::JoinString(segments, " >> ");
}

int IndexAmongSameTagSiblingsCore(const blink::WebElement& element) {
  int index = 1;
  for (blink::WebNode sibling = element.PreviousSibling(); !sibling.IsNull();
       sibling = sibling.PreviousSibling()) {
    if (!sibling.IsElementNode()) {
      continue;
    }
    blink::WebElement sibling_element = sibling.To<blink::WebElement>();
    if (sibling_element.TagName().Utf8() == element.TagName().Utf8()) {
      ++index;
    }
  }
  return index;
}

std::string BuildResourcePathCore(const blink::WebElement& element) {
  std::vector<std::string> segments;
  blink::WebNode node = element;
  while (!node.IsNull()) {
    if (!node.IsElementNode()) {
      break;
    }
    blink::WebElement el = node.To<blink::WebElement>();
    std::string tag = LowerTagName(el);
    int index = IndexAmongSameTagSiblingsCore(el);
    segments.push_back(tag + "[" + base::NumberToString(index) + "]");
    node = el.ParentOrShadowHostNode();
  }
  if (segments.empty())
    return std::string();
  std::reverse(segments.begin(), segments.end());
  return std::string("/") + base::JoinString(segments, "/");
}

bool IsHtmlOrBody(const blink::WebElement& el) {
  if (el.IsNull())
    return false;
  return el.HasHTMLTagName(blink::WebString::FromUTF8("html")) ||
         el.HasHTMLTagName(blink::WebString::FromUTF8("body"));
}

bool IsInteractiveCandidate(const blink::WebElement& el) {
  if (el.IsNull() || IsHtmlOrBody(el))
    return false;

  // Prefer AX state when available.
  AXSnapshot ax = GetAXSnapshot(el);
  if (ax.valid && !ax.ignored) {
    if (ax.clickable || ax.focusable)
      return true;
  }

  if (el.IsFormControlElement() || el.IsEditable())
    return true;

  if (el.HasHTMLTagName(blink::WebString::FromUTF8("a")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("button")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("input")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("textarea")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("select")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("label")) ||
      el.HasHTMLTagName(blink::WebString::FromUTF8("summary"))) {
    return true;
  }

  if (el.HasAttribute(blink::WebString::FromUTF8("onclick")))
    return true;

  std::string role =
      el.GetAttribute(blink::WebString::FromUTF8("role")).Utf8();
  role = base::ToLowerASCII(role);
  if (role == "button" || role == "link")
    return true;

  return false;
}

blink::WebElement PromoteToInteractiveAncestor(blink::WebElement el) {
  if (el.IsNull())
    return el;

  blink::WebElement cur = el;
  for (int i = 0; i < 6 && !cur.IsNull(); ++i) {
    if (IsInteractiveCandidate(cur))
      return cur;

    blink::WebNode parent = cur.ParentNode();
    if (parent.IsNull() || !parent.IsElementNode())
      break;
    cur = parent.To<blink::WebElement>();
  }

  return el;
}

std::string NormalizeTextForLocator(std::string text, size_t max_bytes) {
  text = base::CollapseWhitespaceASCII(text,
                                      /*trim_sequences_with_line_breaks=*/true);
  std::string out;
  base::TruncateUTF8ToByteSize(text, max_bytes, &out);
  return out;
}

std::vector<std::string> SplitLocatorTokens(std::string_view value) {
  return base::SplitString(std::string(value), " \t\r\n", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

bool ClassListContainsToken(const blink::WebElement& element,
                            std::string_view wanted) {
  if (element.IsNull() || wanted.empty()) {
    return false;
  }
  const std::vector<std::string> tokens = SplitLocatorTokens(
      element.GetAttribute(blink::WebString::FromUTF8("class")).Utf8());
  const std::string wanted_lower = base::ToLowerASCII(std::string(wanted));
  for (const std::string& token : tokens) {
    if (base::ToLowerASCII(token) == wanted_lower) {
      return true;
    }
  }
  return false;
}

blink::WebElement FirstMatchingElement(
    blink::WebDocument& document,
    const base::RepeatingCallback<bool(const blink::WebElement&)>& predicate) {
  blink::WebElementCollection elements = document.All();
  for (blink::WebElement element = elements.FirstItem(); !element.IsNull();
       element = elements.NextItem()) {
    if (!predicate.Run(element)) {
      continue;
    }
    return element;
  }
  return blink::WebElement();
}

blink::WebElement ResolveShadowChain(blink::WebDocument& document,
                                     std::string_view chain) {
  std::vector<std::string> segments = base::SplitString(
      std::string(chain), ">", base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);
  std::vector<std::string> cleaned;
  cleaned.reserve(segments.size());
  for (const std::string& segment : segments) {
    std::string trimmed = segment;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    if (!trimmed.empty()) {
      if (trimmed == ">") {
        continue;
      }
      cleaned.push_back(trimmed);
    }
  }
  if (cleaned.empty()) {
    return blink::WebElement();
  }

  blink::WebElement current =
      document.QuerySelector(blink::WebString::FromUTF8(cleaned.front()));
  if (current.IsNull()) {
    return blink::WebElement();
  }
  for (size_t i = 1; i < cleaned.size(); ++i) {
    blink::WebNode shadow_root = current.ShadowRoot();
    if (shadow_root.IsNull()) {
      return blink::WebElement();
    }
    blink::WebElement next = shadow_root.QuerySelector(
        blink::WebString::FromUTF8(cleaned[i]));
    if (!next) {
      return blink::WebElement();
    }
    current = next;
  }
  return current;
}

blink::WebElement FindByName(blink::WebDocument& document,
                             std::string_view name) {
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted, const blink::WebElement& element) {
            return element.GetAttribute(blink::WebString::FromUTF8("name"))
                       .Utf8() == wanted;
          },
          std::string(name)));
}

blink::WebElement FindByClassName(blink::WebDocument& document,
                                  std::string_view class_name) {
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted, const blink::WebElement& element) {
            return ClassListContainsToken(element, wanted);
          },
          std::string(class_name)));
}

blink::WebElement FindByTagName(blink::WebDocument& document,
                                std::string_view tag_name) {
  const std::string wanted = base::ToLowerASCII(std::string(tag_name));
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted_tag, const blink::WebElement& element) {
            if (element.IsNull()) {
              return false;
            }
            return base::ToLowerASCII(element.TagName().Utf8()) == wanted_tag;
          },
          wanted));
}

blink::WebElement FindByAccessibilityName(blink::WebDocument& document,
                                          std::string_view ax_name) {
  const std::string wanted = NormalizeTextForLocator(std::string(ax_name), 256u);
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted_name, const blink::WebElement& element) {
            AXSnapshot ax = GetAXSnapshot(element);
            if (!ax.valid || ax.ignored || ax.name.empty()) {
              return false;
            }
            return NormalizeTextForLocator(ax.name, 256u) == wanted_name;
          },
          wanted));
}

blink::WebElement FindByTextContent(blink::WebDocument& document,
                                    std::string_view text) {
  const std::string wanted = NormalizeTextForLocator(std::string(text), 256u);
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted_text, const blink::WebElement& element) {
            return NormalizeTextForLocator(
                       base::UTF16ToUTF8(element.TextContent().Utf16()), 256u) ==
                   wanted_text;
          },
          wanted));
}

blink::WebElement FindByLinkText(blink::WebDocument& document,
                                 std::string_view text,
                                 bool partial_match) {
  const std::string wanted = NormalizeTextForLocator(std::string(text), 256u);
  return FirstMatchingElement(
      document,
      base::BindRepeating(
          [](std::string wanted_text, bool partial,
             const blink::WebElement& element) {
            if (element.IsNull() ||
                !element.HasHTMLTagName(blink::WebString::FromUTF8("a"))) {
              return false;
            }
            const std::string current = NormalizeTextForLocator(
                base::UTF16ToUTF8(element.TextContent().Utf16()), 256u);
            return partial ? base::Contains(current, wanted_text)
                           : current == wanted_text;
          },
          wanted, partial_match));
}

std::string LowerAttr(const blink::WebElement& el, const char* name) {
  return base::ToLowerASCII(
      el.GetAttribute(blink::WebString::FromUTF8(name)).Utf8());
}

std::vector<std::string> StableClassTokens(const blink::WebElement& el) {
  std::vector<std::string> out;
  for (const std::string& token :
       base::SplitString(el.GetAttribute(blink::WebString::FromUTF8("class"))
                             .Utf8(),
                         " \t\r\n", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    if (!IsLikelyGeneratedIdentifierToken(token)) {
      out.push_back(token);
    }
  }
  return out;
}

blink::WebElement ParentElement(const blink::WebElement& el) {
  blink::WebNode parent = el.ParentNode();
  return !parent.IsNull() && parent.IsElementNode()
             ? parent.To<blink::WebElement>()
             : blink::WebElement();
}

blink::WebElement ClosestMatching(
    blink::WebElement el,
    const base::RepeatingCallback<bool(const blink::WebElement&)>& pred) {
  for (int depth = 0; !el.IsNull() && depth < 12; ++depth) {
    if (pred.Run(el)) {
      return el;
    }
    el = ParentElement(el);
  }
  return blink::WebElement();
}

// Mirrors how a person reads a list: same tag, same role, or a shared class
// marks siblings as items of one list. li/tr/row roles are strong signals.
int ScorePeer(const blink::WebElement& focus, const blink::WebElement& node) {
  const std::string focus_tag = LowerTagName(focus);
  const std::string node_tag = LowerTagName(node);
  int score = 0;
  if (node_tag == focus_tag) {
    score += 2;
  }
  const std::string focus_role = LowerAttr(focus, "role");
  if (!focus_role.empty() && LowerAttr(node, "role") == focus_role) {
    score += 2;
  }
  const std::vector<std::string> focus_classes = StableClassTokens(focus);
  for (const std::string& c : StableClassTokens(node)) {
    if (base::Contains(focus_classes, c)) {
      score += 1;
      break;
    }
  }
  if ((focus_tag == "li" || focus_tag == "tr") && node_tag == focus_tag) {
    score += 3;
  }
  if ((focus_role == "row" || focus_role == "listitem") &&
      LowerAttr(node, "role") == focus_role) {
    score += 3;
  }
  return score;
}

std::string SelectorForElementItself(const blink::WebElement& el) {
  const std::string id = el.GetIdAttribute().Utf8();
  if (!id.empty() && !IsLikelyGeneratedIdentifierToken(id)) {
    return "#" + id;
  }
  if (std::optional<std::pair<std::string, std::string>> attr =
          PreferredStableAttr(el)) {
    return BuildCssSelectorForAttr(attr->first, attr->second);
  }
  std::string css = LowerTagName(el);
  const std::vector<std::string> classes = StableClassTokens(el);
  if (!classes.empty()) {
    css += "." + classes.front();
  }
  return css;
}

std::string RelativeCssPath(const blink::WebElement& ancestor,
                            blink::WebElement el) {
  std::vector<std::string> segments;
  for (int depth = 0; !el.IsNull() && el != ancestor && depth < 12; ++depth) {
    int index = 1;
    for (blink::WebNode sib = el.PreviousSibling(); !sib.IsNull();
         sib = sib.PreviousSibling()) {
      if (sib.IsElementNode()) {
        ++index;
      }
    }
    segments.insert(segments.begin(), base::StringPrintf(
                                          "%s:nth-child(%d)",
                                          LowerTagName(el).c_str(), index));
    el = ParentElement(el);
  }
  if (el != ancestor) {
    return std::string();
  }
  return base::JoinString(segments, " > ");
}

}  // namespace

StealthyElementResolver::StealthyElementResolver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame), render_frame_(render_frame) {
  if (!render_frame_)
    return;
  render_frame_->GetAssociatedInterfaceRegistry()->AddInterface<
      dashboard::mojom::ElementResolver>(
      base::BindRepeating(&StealthyElementResolver::BindElementResolver,
                          weak_factory_.GetWeakPtr()));
  render_frame_->GetAssociatedInterfaceRegistry()->AddInterface<
      dashboard::mojom::StealthyElementResolver>(
      base::BindRepeating(&StealthyElementResolver::BindStealthyResolver,
                          weak_factory_.GetWeakPtr()));
}

StealthyElementResolver::~StealthyElementResolver() = default;

void StealthyElementResolver::OnDestruct() {
  delete this;
}

void StealthyElementResolver::BindElementResolver(
    mojo::PendingAssociatedReceiver<dashboard::mojom::ElementResolver> receiver) {
  element_receivers_.Add(this, std::move(receiver));
}

void StealthyElementResolver::BindStealthyResolver(
    mojo::PendingAssociatedReceiver<dashboard::mojom::StealthyElementResolver> receiver) {
  stealth_receivers_.Add(this, std::move(receiver));
}

void StealthyElementResolver::ResolveElementStealthily(
    std::vector<dashboard::mojom::LocatorPtr> locators,
    dashboard::mojom::SearchScope scope,
    ResolveElementStealthilyCallback callback) {
  if (!render_frame_) {
    std::move(callback).Run(nullptr);
    return;
  }
  blink::WebFrame* web_frame = render_frame_->GetWebFrame();
  blink::WebLocalFrame* frame =
      web_frame && web_frame->IsWebLocalFrame()
          ? web_frame->ToWebLocalFrame()
          : nullptr;
  if (!frame) {
    std::move(callback).Run(nullptr);
    return;
  }
  blink::WebDocument document = frame->GetDocument();
  if (document.IsNull()) {
    std::move(callback).Run(nullptr);
    return;
  }

  element_cache_.clear();

  blink::WebElement element;

  for (const auto& locator : locators) {
    if (locator->value.empty())
      continue;

    const std::string locator_type = base::ToUpperASCII(locator->type);
    blink::WebString query = blink::WebString::FromUTF8(locator->value);

    if (locator_type == "FRAME_ID") {
      const std::string frame_token =
          frame->GetLocalFrameToken().ToString();
      if (locator->value != frame_token) {
        continue;
      }
      element = document.FocusedElement();
      if (element.IsNull()) {
        continue;
      }
    } else if (locator_type == "CSS_SELECTOR") {
      element = document.QuerySelector(query);
    } else if (locator_type == "ID") {
      element = document.GetElementById(query);
    } else if (locator_type == "NAME") {
      element = FindByName(document, locator->value);
    } else if (locator_type == "CLASS_NAME") {
      element = FindByClassName(document, locator->value);
    } else if (locator_type == "TAG_NAME") {
      element = FindByTagName(document, locator->value);
    } else if (locator_type == "LINK_TEXT") {
      element = FindByLinkText(document, locator->value, /*partial_match=*/false);
    } else if (locator_type == "PARTIAL_LINK_TEXT") {
      element = FindByLinkText(document, locator->value, /*partial_match=*/true);
    } else if (locator_type == "ACCESSIBILITY_NAME") {
      element = FindByAccessibilityName(document, locator->value);
    } else if (locator_type == "TEXT_CONTENT") {
      element = FindByTextContent(document, locator->value);
    } else if (locator_type == "SHADOW_DOM") {
      element = ResolveShadowChain(document, locator->value);
    } else if (locator_type == "XPATH") {
      element = FindByXPath(document, locator->value);
    } else {
      LOG(WARNING) << "[StealthyResolver] Unsupported locator type: "
                   << locator_type;
    }

    if (scope == dashboard::mojom::SearchScope::kInteractiveOnly) {
      element = PromoteToInteractiveAncestor(element);
      if (!element.IsNull() && !IsInteractiveCandidate(element)) {
        element = blink::WebElement();
      }
    }

    if (!element.IsNull()) {
      auto found = BuildResolvedElement(element);
      std::move(callback).Run(std::move(found));
      return;
    }
  }

  std::move(callback).Run(nullptr);
}

void StealthyElementResolver::ResolveAtPoint(int32_t x,
                                             int32_t y,
                                             ResolveAtPointCallback callback) {
  auto info = dashboard::mojom::ResolvedElementInfo::New();
  info->ok = false;
  info->node_id = 0;
  info->frame_routing_id = 0;
  info->center_x = x;
  info->center_y = y;
  info->frame_id = std::string();

  if (!render_frame_) {
    std::move(callback).Run(std::move(info));
    return;
  }
  blink::WebView* view = render_frame_->GetWebView();
  if (!view) {
    std::move(callback).Run(std::move(info));
    return;
  }
  blink::WebFrameWidget* widget = view->MainFrameWidget();
  if (!widget) {
    std::move(callback).Run(std::move(info));
    return;
  }

  blink::WebHitTestResult hit = widget->HitTestResultAt(
      // InputObserver / recorder pass CSS/DIP widget coordinates. HitTestResultAt
      // expects Blink space (DIPs * device_scale_factor). Without this conversion on
      // Retina, a click on Log in (y≈550 DIP) is hit-tested at y≈550 Blink ≈
      // y≈275 CSS and resolves the Apple button above it.
      widget->DIPsToBlinkSpace(gfx::PointF(x, y)));
  if (hit.IsNull()) {
    std::move(callback).Run(std::move(info));
    return;
  }

  // Prefer the deepest node under the cursor, then promote to an interactive
  // ancestor. Do NOT prefer UrlElement() first: an enclosing/nearby <a> (e.g.
  // Apple OAuth) can steal recording from a distinct button the user clicked
  // (e.g. Log in) when hit-test geometry is ambiguous.
  blink::WebElement element;
  blink::WebNode node = hit.GetNodeOrPseudoNode();
  if (!node.IsNull()) {
    blink::WebNode cur = node;
    for (int i = 0; i < 8 && !cur.IsNull(); ++i) {
      if (cur.IsElementNode()) {
        element = cur.To<blink::WebElement>();
        break;
      }
      cur = cur.ParentNode();
    }
  }
  if (element.IsNull()) {
    element = hit.GetElement();
  }
  if ((element.IsNull() || IsHtmlOrBody(element))) {
    blink::WebElement url_el = hit.UrlElement();
    if (!url_el.IsNull()) {
      element = url_el;
    }
  }
  if (element.IsNull()) {
    std::move(callback).Run(std::move(info));
    return;
  }

  element = PromoteToInteractiveAncestor(element);

  if (IsHtmlOrBody(element)) {
    // Returning BODY/HTML is almost never helpful for recording. Force fallback
    // to the browser-side probe which can use elementFromPoint/closest().
    std::move(callback).Run(std::move(info));
    return;
  }

  // Reject results whose box does not contain the click (DIP space).
  {
    gfx::RectF bounds_blink(element.BoundsInWidget());
    gfx::RectF bounds_dip = widget->BlinkSpaceToDIPs(bounds_blink);
    bounds_dip.Outset(4.0f);
    if (!bounds_dip.IsEmpty() &&
        !bounds_dip.Contains(gfx::PointF(x, y))) {
      LOG(INFO) << "[StealthyResolver] ResolveAtPoint rejecting out-of-bounds "
                << "hit tag=" << element.TagName().Utf8() << " point_dip=" << x
                << "," << y << " bounds_dip=" << bounds_dip.ToString();
      std::move(callback).Run(std::move(info));
      return;
    }
  }

  auto resolved = BuildResolvedElement(element);
  resolved->center_x = x;
  resolved->center_y = y;
  std::move(callback).Run(std::move(resolved));
}

void StealthyElementResolver::CollectAllRelevantElements(
    dashboard::mojom::SearchScope scope,
    CollectAllRelevantElementsCallback callback) {
  std::vector<dashboard::mojom::ResolvedElementInfoPtr> results;

  if (!render_frame_) {
    std::move(callback).Run(std::move(results));
    return;
  }
  blink::WebFrame* web_frame = render_frame_->GetWebFrame();
  blink::WebLocalFrame* frame =
      web_frame && web_frame->IsWebLocalFrame()
          ? web_frame->ToWebLocalFrame()
          : nullptr;
  if (!frame) {
    std::move(callback).Run(std::move(results));
    return;
  }
  blink::WebDocument doc = frame->GetDocument();
  if (doc.IsNull()) {
    std::move(callback).Run(std::move(results));
    return;
  }

  element_cache_.clear();
  next_node_id_ = 0;

  blink::WebElementCollection elements = doc.All();
  for (blink::WebElement element = elements.FirstItem();
       !element.IsNull();
       element = elements.NextItem()) {
    if (element.IsNull())
      continue;

    AXSnapshot ax = GetAXSnapshot(element);
    if (ax.valid && ax.ignored)
      continue;

    bool focusable = ax.valid ? ax.focusable
                              : (element.IsFormControlElement() ||
                                 element.HasAttribute(
                                     blink::WebString::FromUTF8("tabindex")));
    bool clickable =
        ax.valid
            ? ax.clickable
            : (focusable ||
               element.HasHTMLTagName(blink::WebString::FromUTF8("a")) ||
               element.HasHTMLTagName(blink::WebString::FromUTF8("button")));
    if (scope == dashboard::mojom::SearchScope::kInteractiveOnly &&
        !focusable && !clickable) {
      continue;
    }

    auto info = BuildResolvedElement(element);
    if (!info->ok)
      continue;
    // Ensure AX states override heuristics.
    info->clickable = clickable;
    info->focusable = focusable;
    results.push_back(std::move(info));
  }

  std::move(callback).Run(std::move(results));
}

void StealthyElementResolver::ResolveAllMatches(
    std::vector<dashboard::mojom::LocatorPtr> locators,
    dashboard::mojom::SearchScope scope,
    uint32_t max_results,
    ResolveAllMatchesCallback callback) {
  std::vector<dashboard::mojom::ResolvedElementInfoPtr> results;
  blink::WebFrame* web_frame =
      render_frame_ ? render_frame_->GetWebFrame() : nullptr;
  blink::WebLocalFrame* frame =
      web_frame && web_frame->IsWebLocalFrame() ? web_frame->ToWebLocalFrame()
                                                : nullptr;
  blink::WebDocument document = frame ? frame->GetDocument() : blink::WebDocument();
  if (document.IsNull()) {
    std::move(callback).Run(std::move(results));
    return;
  }
  const size_t cap = std::clamp<size_t>(max_results, 1u, 1024u);

  std::vector<blink::WebElement> matches;
  auto add = [&](blink::WebElement element) {
    if (element.IsNull() || matches.size() >= cap) {
      return;
    }
    if (scope == dashboard::mojom::SearchScope::kInteractiveOnly) {
      element = PromoteToInteractiveAncestor(element);
      if (element.IsNull() || !IsInteractiveCandidate(element)) {
        return;
      }
    }
    if (std::find(matches.begin(), matches.end(), element) != matches.end()) {
      return;
    }
    matches.push_back(element);
  };
  // Text predicates match every ancestor with the same text; keep the
  // innermost element by skipping ones whose child element also matches.
  auto collect_innermost =
      [&](const base::RepeatingCallback<bool(const blink::WebElement&)>& pred) {
        blink::WebElementCollection all = document.All();
        for (blink::WebElement el = all.FirstItem(); !el.IsNull();
             el = all.NextItem()) {
          if (matches.size() >= cap) {
            return;
          }
          if (!pred.Run(el)) {
            continue;
          }
          bool child_matches = false;
          for (blink::WebNode child = el.FirstChild(); !child.IsNull();
               child = child.NextSibling()) {
            if (child.IsElementNode() &&
                pred.Run(child.To<blink::WebElement>())) {
              child_matches = true;
              break;
            }
          }
          if (!child_matches) {
            add(el);
          }
        }
      };
  auto collect_all =
      [&](const base::RepeatingCallback<bool(const blink::WebElement&)>& pred) {
        blink::WebElementCollection all = document.All();
        for (blink::WebElement el = all.FirstItem(); !el.IsNull();
             el = all.NextItem()) {
          if (matches.size() >= cap) {
            return;
          }
          if (pred.Run(el)) {
            add(el);
          }
        }
      };

  for (const auto& locator : locators) {
    if (locator->value.empty() || matches.size() >= cap) {
      continue;
    }
    const std::string type = base::ToUpperASCII(locator->type);
    const std::string& value = locator->value;
    if (type == "CSS_SELECTOR") {
      for (const blink::WebElement& el :
           document.QuerySelectorAll(blink::WebString::FromUTF8(value))) {
        add(el);
      }
    } else if (type == "ID") {
      add(document.GetElementById(blink::WebString::FromUTF8(value)));
    } else if (type == "SHADOW_DOM") {
      add(ResolveShadowChain(document, value));
    } else if (type == "NAME") {
      collect_all(base::BindRepeating(
          [](std::string wanted, const blink::WebElement& el) {
            return el.GetAttribute(blink::WebString::FromUTF8("name")).Utf8() ==
                   wanted;
          },
          value));
    } else if (type == "CLASS_NAME") {
      collect_all(base::BindRepeating(
          [](std::string wanted, const blink::WebElement& el) {
            return ClassListContainsToken(el, wanted);
          },
          value));
    } else if (type == "TAG_NAME") {
      collect_all(base::BindRepeating(
          [](std::string wanted, const blink::WebElement& el) {
            return base::ToLowerASCII(el.TagName().Utf8()) == wanted;
          },
          base::ToLowerASCII(value)));
    } else if (type == "TEXT_CONTENT") {
      collect_innermost(base::BindRepeating(
          [](std::string wanted, const blink::WebElement& el) {
            return NormalizeTextForLocator(
                       base::UTF16ToUTF8(el.TextContent().Utf16()), 256u) ==
                   wanted;
          },
          NormalizeTextForLocator(value, 256u)));
    } else if (type == "LINK_TEXT" || type == "PARTIAL_LINK_TEXT") {
      collect_all(base::BindRepeating(
          [](std::string wanted, bool partial, const blink::WebElement& el) {
            if (!el.HasHTMLTagName(blink::WebString::FromUTF8("a"))) {
              return false;
            }
            const std::string text = NormalizeTextForLocator(
                base::UTF16ToUTF8(el.TextContent().Utf16()), 256u);
            return partial ? base::Contains(text, wanted) : text == wanted;
          },
          NormalizeTextForLocator(value, 256u), type == "PARTIAL_LINK_TEXT"));
    } else if (type == "ACCESSIBILITY_NAME") {
      collect_innermost(base::BindRepeating(
          [](std::string wanted, const blink::WebElement& el) {
            AXSnapshot ax = GetAXSnapshot(el);
            return ax.valid && !ax.ignored && !ax.name.empty() &&
                   NormalizeTextForLocator(ax.name, 256u) == wanted;
          },
          NormalizeTextForLocator(value, 256u)));
    }
  }

  element_cache_.clear();
  for (blink::WebElement& el : matches) {
    auto info = BuildResolvedElement(el);
    if (info->ok) {
      info->inner_html.clear();
      results.push_back(std::move(info));
    }
  }
  std::move(callback).Run(std::move(results));
}

void StealthyElementResolver::FindRepeatedPeersAtPoint(
    int32_t x,
    int32_t y,
    uint32_t max_peers,
    FindRepeatedPeersAtPointCallback callback) {
  blink::WebView* view = render_frame_ ? render_frame_->GetWebView() : nullptr;
  blink::WebFrameWidget* widget = view ? view->MainFrameWidget() : nullptr;
  if (!widget) {
    std::move(callback).Run(nullptr);
    return;
  }
  blink::WebHitTestResult hit =
      widget->HitTestResultAt(widget->DIPsToBlinkSpace(gfx::PointF(x, y)));
  if (hit.IsNull()) {
    std::move(callback).Run(nullptr);
    return;
  }
  blink::WebElement target;
  for (blink::WebNode cur = hit.GetNodeOrPseudoNode(); !cur.IsNull();
       cur = cur.ParentNode()) {
    if (cur.IsElementNode()) {
      target = cur.To<blink::WebElement>();
      break;
    }
  }
  if (target.IsNull()) {
    target = hit.GetElement();
  }
  if (target.IsNull() || IsHtmlOrBody(target)) {
    std::move(callback).Run(nullptr);
    return;
  }

  auto has_tag = [](const char* tag) {
    return base::BindRepeating(
        [](const char* t, const blink::WebElement& el) {
          return LowerTagName(el) == t;
        },
        tag);
  };
  auto has_role = [](const char* role) {
    return base::BindRepeating(
        [](const char* r, const blink::WebElement& el) {
          return LowerAttr(el, "role") == r;
        },
        role);
  };
  // Rows win over the clicked cell so a click inside a table or list selects
  // the row as the repeated item rather than sibling cells.
  std::vector<blink::WebElement> focus_candidates = {
      ClosestMatching(target, has_tag("li")),
      ClosestMatching(target, has_tag("tr")),
      ClosestMatching(target, has_role("row")),
      ClosestMatching(target, has_role("listitem")),
      target,
      ParentElement(target),
  };

  const size_t limit = std::clamp<size_t>(max_peers, 2u, 200u);
  blink::WebElement focus;
  std::vector<blink::WebElement> peers;
  for (const blink::WebElement& candidate : focus_candidates) {
    if (candidate.IsNull() || IsHtmlOrBody(candidate)) {
      continue;
    }
    blink::WebElement parent = ParentElement(candidate);
    if (parent.IsNull()) {
      continue;
    }
    std::vector<blink::WebElement> found;
    for (blink::WebNode child = parent.FirstChild();
         !child.IsNull() && found.size() < limit; child = child.NextSibling()) {
      if (!child.IsElementNode()) {
        continue;
      }
      blink::WebElement el = child.To<blink::WebElement>();
      const gfx::RectF dip = widget->BlinkSpaceToDIPs(
          gfx::RectF(el.BoundsInWidget()));
      if (dip.width() < 4 || dip.height() < 4 || ScorePeer(candidate, el) <= 0) {
        continue;
      }
      found.push_back(el);
    }
    if (found.size() >= 2) {
      focus = candidate;
      peers = std::move(found);
      break;
    }
  }
  if (focus.IsNull()) {
    std::move(callback).Run(nullptr);
    return;
  }

  // Pick the narrowest selector that still matches every peer.
  blink::WebDocument document = render_frame_->GetWebFrame()->GetDocument();
  auto matches_all_peers = [&](const std::string& css) {
    if (css.empty()) {
      return false;
    }
    std::set<blink::WebElement> matched;
    for (const blink::WebElement& el :
         document.QuerySelectorAll(blink::WebString::FromUTF8(css))) {
      matched.insert(el);
    }
    return std::ranges::all_of(peers, [&](const blink::WebElement& p) {
      return matched.contains(p);
    });
  };
  const std::string focus_tag = LowerTagName(focus);
  std::string item = focus_tag;
  const std::string focus_role = LowerAttr(focus, "role");
  if (!focus_role.empty()) {
    item += "[role=\"" + focus_role + "\"]";
  }
  std::vector<std::string> shared_classes = StableClassTokens(focus);
  std::erase_if(shared_classes, [&](const std::string& c) {
    return !std::ranges::all_of(peers, [&](const blink::WebElement& p) {
      return base::Contains(StableClassTokens(p), c);
    });
  });
  const std::string parent_css =
      SelectorForElementItself(ParentElement(focus));
  std::vector<std::string> candidates;
  if (!shared_classes.empty()) {
    candidates.push_back(parent_css + " > " + item + "." +
                         shared_classes.front());
  }
  candidates.push_back(parent_css + " > " + item);
  if (!shared_classes.empty()) {
    candidates.push_back(item + "." + shared_classes.front());
  }
  std::string locator;
  for (const std::string& css : candidates) {
    if (matches_all_peers(css)) {
      locator = css;
      break;
    }
  }
  if (locator.empty()) {
    std::move(callback).Run(nullptr);
    return;
  }

  auto result = dashboard::mojom::RepeatedPeers::New();
  result->locator = locator;
  result->relative_locator = RelativeCssPath(focus, target);
  result->target = BuildResolvedElement(target);
  for (const blink::WebElement& el : peers) {
    const gfx::Rect dip = gfx::ToEnclosingRect(
        widget->BlinkSpaceToDIPs(gfx::RectF(el.BoundsInWidget())));
    auto info = dashboard::mojom::ResolvedElementInfo::New();
    info->ok = true;
    info->tag = LowerTagName(el);
    info->x = dip.x();
    info->y = dip.y();
    info->width = dip.width();
    info->height = dip.height();
    info->center_x = dip.CenterPoint().x();
    info->center_y = dip.CenterPoint().y();
    result->peers.push_back(std::move(info));
  }
  std::move(callback).Run(std::move(result));
}

void StealthyElementResolver::InvestigatePage(
    uint32_t max_links,
    uint32_t max_inputs,
    uint32_t max_buttons,
    uint32_t max_text_chars,
    InvestigatePageCallback callback) {
  base::Value::Dict out;
  blink::WebLocalFrame* frame =
      render_frame_ ? render_frame_->GetWebFrame() : nullptr;
  if (!frame) {
    std::move(callback).Run(std::move(out));
    return;
  }
  blink::WebDocument document = frame->GetDocument();
  auto clip = [](const blink::WebString& s, size_t n) {
    std::string t(base::CollapseWhitespaceASCII(
        s.Utf8(), /*trim_sequences_with_line_breaks=*/true));
    if (t.size() > n) {
      t.resize(n);
    }
    return t;
  };
  auto attr = [](const blink::WebElement& el, const char* name) {
    return el.GetAttribute(blink::WebString::FromUTF8(name)).Utf8();
  };
  auto query = [&](const char* css) {
    return document.QuerySelectorAll(blink::WebString::FromUTF8(css));
  };

  base::Value::List links;
  for (const blink::WebElement& el : query("a[href]")) {
    if (links.size() >= std::max<uint32_t>(1, max_links)) {
      break;
    }
    std::string href(base::TrimWhitespaceASCII(attr(el, "href"),
                                               base::TRIM_ALL));
    if (href.empty()) {
      continue;
    }
    links.Append(base::Value::Dict()
                     .Set("href", href)
                     .Set("text", clip(el.TextContent(), 140)));
  }

  base::Value::List inputs;
  for (const blink::WebElement& el : query("input,textarea,select")) {
    if (inputs.size() >= std::max<uint32_t>(1, max_inputs)) {
      break;
    }
    inputs.Append(base::Value::Dict()
                      .Set("tag", LowerTagName(el))
                      .Set("type", attr(el, "type"))
                      .Set("name", attr(el, "name"))
                      .Set("id", attr(el, "id"))
                      .Set("placeholder", attr(el, "placeholder"))
                      .Set("aria", attr(el, "aria-label")));
  }

  base::Value::List buttons;
  for (const blink::WebElement& el :
       query("button,input[type=\"submit\"],input[type=\"button\"],"
             "[role=\"button\"]")) {
    if (buttons.size() >= std::max<uint32_t>(1, max_buttons)) {
      break;
    }
    std::string text = clip(el.TextContent(), 140);
    if (text.empty()) {
      text = clip(blink::WebString::FromUTF8(attr(el, "value")), 140);
    }
    buttons.Append(base::Value::Dict()
                       .Set("text", text)
                       .Set("id", attr(el, "id"))
                       .Set("className", attr(el, "class")));
  }

  base::Value::List tables;
  for (const blink::WebElement& table : query("table")) {
    if (tables.size() >= 6) {
      break;
    }
    blink::WebElement tbody;
    for (blink::WebNode n = table.FirstChild(); !n.IsNull();
         n = n.NextSibling()) {
      if (n.IsElementNode() &&
          LowerTagName(n.To<blink::WebElement>()) == "tbody") {
        tbody = n.To<blink::WebElement>();
        break;
      }
    }
    base::Value::List child_tags;
    base::Value::List sample_rows;
    int child_count = 0;
    int tr_count = 0;
    if (!tbody.IsNull()) {
      std::set<std::string> seen;
      for (blink::WebNode n = tbody.FirstChild(); !n.IsNull();
           n = n.NextSibling()) {
        if (!n.IsElementNode()) {
          continue;
        }
        blink::WebElement child = n.To<blink::WebElement>();
        const std::string tag = LowerTagName(child);
        ++child_count;
        if (seen.insert(tag).second) {
          child_tags.Append(tag);
        }
        if (tag == "tr") {
          ++tr_count;
          if (sample_rows.size() < 3) {
            sample_rows.Append(clip(child.TextContent(), 120));
          }
        }
      }
    }
    const std::string id = table.GetIdAttribute().Utf8();
    std::string class_name = attr(table, "class");
    if (class_name.size() > 80) {
      class_name.resize(80);
    }
    tables.Append(
        base::Value::Dict()
            .Set("id", id)
            .Set("className", class_name)
            .Set("tbodyChildTags", std::move(child_tags))
            .Set("tbodyChildCount", child_count)
            .Set("trCount", tr_count)
            .Set("sampleRows", std::move(sample_rows))
            .Set("suggestedRowSelector",
                 id.empty() ? "table tbody tr" : "#" + id + " tbody tr"));
  }

  const size_t text_limit = std::max<uint32_t>(200, max_text_chars);
  out.Set("pageUrl", document.Url().GetString().Utf8());
  out.Set("title", document.Title().Utf8());
  out.Set("readyState", frame->IsLoading() ? "loading" : "complete");
  out.Set("elementCount", static_cast<int>(query("*").size()));
  out.Set("links", std::move(links));
  out.Set("inputs", std::move(inputs));
  out.Set("buttons", std::move(buttons));
  out.Set("tables", std::move(tables));
  out.Set("innerTextExcerpt",
          clip(blink::WebFrameContentDumper::DumpFrameTreeAsText(frame,
                                                                 text_limit),
               text_limit));
  std::move(callback).Run(std::move(out));
}

void StealthyElementResolver::GetElementFeatures(
    int32_t node_id,
    GetElementFeaturesCallback callback) {
  auto features = dashboard::mojom::ElementFeatures::New();

  auto it = element_cache_.find(node_id);
  if (it == element_cache_.end()) {
    LOG(WARNING) << "[StealthyResolver] GetElementFeatures: Node ID "
                 << node_id << " not in cache.";
    std::move(callback).Run(std::move(features));
    return;
  }

  blink::WebElement element = it->second;
  if (element.IsNull()) {
    std::move(callback).Run(std::move(features));
    return;
  }

  gfx::Rect bounds = element.BoundsInWidget();
  if (render_frame_) {
    bounds = render_frame_->ConvertViewportToWindow(bounds);
  }
  features->visible = !bounds.IsEmpty();
  features->interactive =
      element.IsEditable() || element.IsFormControlElement();
  features->has_event_listeners = false;  // TODO: inspect listeners.
  features->hover_changes_appearance = false;  // TODO: simulate hover.
  features->opacity = 1.0;  // TODO: computed style.
  features->width = bounds.width();
  features->height = bounds.height();
  features->shadow_depth = 0;  // TODO: shadow traversal.
  features->cursor = "auto";   // TODO: computed style.
  features->role = std::string();

  std::move(callback).Run(std::move(features));
}

// --- Private Helpers ---

blink::WebElement StealthyElementResolver::FindByXPath(
    blink::WebDocument& document,
    const std::string& xpath_query) {
  // XPath not implemented in this prototype.
  return blink::WebElement();
}

dashboard::mojom::ResolvedElementInfoPtr
StealthyElementResolver::BuildResolvedElement(blink::WebElement& element) {
  auto info = dashboard::mojom::ResolvedElementInfo::New();
  const bool is_text_entry =
      element.HasHTMLTagName(blink::WebString::FromUTF8("input")) ||
      element.HasHTMLTagName(blink::WebString::FromUTF8("textarea"));

  AXSnapshot ax = GetAXSnapshot(element);
  if (ax.valid && ax.ignored) {
    info->ok = false;
    return info;
  }

  gfx::Rect bounds = element.BoundsInWidget();
  if (render_frame_) {
    bounds = render_frame_->ConvertViewportToWindow(bounds);
  }
  if (bounds.IsEmpty()) {
    blink::WebElement cur = element;
    for (int i = 0; i < 6 && !cur.IsNull(); ++i) {
      blink::WebNode parent = cur.ParentNode();
      if (parent.IsNull() || !parent.IsElementNode()) {
        break;
      }
      cur = parent.To<blink::WebElement>();
      gfx::Rect parent_bounds = cur.BoundsInWidget();
      if (render_frame_) {
        parent_bounds = render_frame_->ConvertViewportToWindow(parent_bounds);
      }
      if (!parent_bounds.IsEmpty()) {
        bounds = parent_bounds;
        break;
      }
    }
  }
  info->x = bounds.x();
  info->y = bounds.y();
  info->width = bounds.width();
  info->height = bounds.height();

  info->center_x = bounds.CenterPoint().x();
  info->center_y = bounds.CenterPoint().y();
  info->frame_routing_id = 0;
  info->frame_id = std::string();
  if (blink::WebLocalFrame* local_frame = render_frame_->GetWebFrame()) {
    info->frame_id = local_frame->GetLocalFrameToken().ToString();
  }

  int32_t node_id = ++next_node_id_;
  info->node_id = node_id;
  element_cache_[node_id] = element;

  info->tag = element.TagName().Utf8();
  info->id_attr = element.GetIdAttribute().Utf8();
  info->class_attr =
      element.GetAttribute(blink::WebString::FromUTF8("class")).Utf8();
  info->name_attr =
      element.GetAttribute(blink::WebString::FromUTF8("name")).Utf8();
  info->aria_label =
      element.GetAttribute(blink::WebString::FromUTF8("aria-label")).Utf8();
  info->accessibility_name.clear();
  info->role.clear();
  info->shadow_chain_selector.clear();
  info->resource_path.clear();
  info->inner_html = base::UTF16ToUTF8(element.InnerHTML().Utf16());
  info->href =
      element.GetAttribute(blink::WebString::FromUTF8("href")).Utf8();
  info->src =
      element.GetAttribute(blink::WebString::FromUTF8("src")).Utf8();
  info->value =
      element.GetAttribute(blink::WebString::FromUTF8("value")).Utf8();
  if (ax.valid) {
    info->accessibility_name = ax.name;
    info->role = ax.role;
  }
  info->text = base::UTF16ToUTF8(element.TextContent().Utf16());
  if (is_text_entry && !info->value.empty()) {
    info->text = info->value;
  }
  if (!info->accessibility_name.empty())
    info->text = info->accessibility_name;
  const std::string normalized_text = NormalizeTextForLocator(info->text, 120u);

  if (!element.IsNull()) {
    const PreferredSelectorChoice preferred =
        ChoosePreferredSelector(element, info->id_attr, info->class_attr);
    info->css = preferred.css;
    info->xpath = preferred.xpath;
  }
  if (info->css.empty() && !info->tag.empty()) {
    std::string tag = info->tag;
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    info->css = tag;
  }
  if (info->xpath.empty() && !info->tag.empty()) {
    std::string tag = info->tag;
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    info->xpath = "//" + tag;
  }

  // Emit locator types that match the browser-side LocatorResolver JS.
  if (!info->id_attr.empty())
    info->locators.push_back(dashboard::mojom::Locator::New("ID", info->id_attr));
  if (!info->name_attr.empty())
    info->locators.push_back(dashboard::mojom::Locator::New("NAME", info->name_attr));
  if (!info->class_attr.empty())
    info->locators.push_back(
        dashboard::mojom::Locator::New("CLASS_NAME", info->class_attr));

  if (!normalized_text.empty())
    info->locators.push_back(
        dashboard::mojom::Locator::New("TEXT_CONTENT", normalized_text));

  if (!info->tag.empty()) {
    std::string tag = info->tag;
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    info->locators.push_back(
        dashboard::mojom::Locator::New("TAG_NAME", tag));

    if (tag == "a" && !normalized_text.empty()) {
      info->locators.push_back(
          dashboard::mojom::Locator::New("LINK_TEXT", normalized_text));
      info->locators.push_back(dashboard::mojom::Locator::New(
          "PARTIAL_LINK_TEXT", NormalizeTextForLocator(normalized_text, 40u)));
    }
  }

  if (!info->accessibility_name.empty()) {
    info->locators.push_back(dashboard::mojom::Locator::New(
        "ACCESSIBILITY_NAME", NormalizeTextForLocator(info->accessibility_name, 120u)));
  }

  if (!element.IsNull()) {
    info->shadow_chain_selector = BuildShadowChainSelectorCore(element);
    info->resource_path = BuildResourcePathCore(element);
    for (const std::string& ancestor_css :
         CollectPromotedStableAncestorCssSelectors(element)) {
      info->locators.push_back(
          dashboard::mojom::Locator::New("CSS_SELECTOR", ancestor_css));
    }
  }

  if (!info->css.empty())
    info->locators.push_back(dashboard::mojom::Locator::New("CSS_SELECTOR", info->css));
  if (!info->xpath.empty())
    info->locators.push_back(dashboard::mojom::Locator::New("XPATH", info->xpath));

  bool clickable = element.IsFormControlElement() ||
                   element.HasHTMLTagName(blink::WebString::FromUTF8("a")) ||
                   element.HasHTMLTagName(blink::WebString::FromUTF8("button"));
  bool focusable = clickable ||
                   element.HasAttribute(blink::WebString::FromUTF8("tabindex"));
  bool disabled =
      element.HasAttribute(blink::WebString::FromUTF8("disabled"));
  if (ax.valid) {
    clickable = ax.clickable;
    focusable = ax.focusable;
    disabled = ax.disabled;
  }
  info->clickable = clickable;
  info->focusable = focusable;
  info->is_disabled = disabled;
  info->visible = !bounds.IsEmpty();
  info->ok = true;
  return info;
}

}  // namespace dashboard_renderer
