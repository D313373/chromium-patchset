// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/actor/tool_utils.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/chrome_features.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_hit_test_result.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/blink/public/web/web_widget.h"
#include "ui/events/base_event_utils.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"

namespace actor {

namespace {

using ::blink::WebNode;
using ::blink::WebWidget;

std::vector<gfx::Rect> getNewHitBoxesFor(gfx::Rect& rect,
                                         const gfx::Rect exclusion) {
  std::vector<gfx::Rect> new_rects;
  // This function only makes sense if at least part of the exclusion falls
  // inside of `rect`.
  DCHECK(rect.Intersects(exclusion));
  DCHECK(!exclusion.Contains(rect));

  // There are 4 possible rects. We handle the top and bottom first so that we
  // can assign the corners to them.

  // The y position and height of the left and right rects. We may have to
  // adjust the y position if the top or bottom are assigned to avoid
  // overlapping.
  int side_y = rect.y();
  int side_height = rect.height();

  // Handle above the exclusion.
  if (rect.y() < exclusion.y()) {
    // Left and right rects will start at the exclusion y position.
    side_y = exclusion.y();
    side_height -= exclusion.y() - rect.y();

    new_rects.emplace_back(rect.x(), rect.y(), rect.width(),
                           exclusion.y() - rect.y());
  }
  // Handle below the exclusion.
  if (rect.bottom() > exclusion.bottom()) {
    // Left and right rects end at the exclusion y position + height.
    side_height -= rect.bottom() - exclusion.bottom();

    new_rects.emplace_back(rect.x(), exclusion.bottom(), rect.width(),
                           rect.bottom() - exclusion.bottom());
  }

  // Handle left of the exclusion.
  if (rect.x() < exclusion.x()) {
    new_rects.emplace_back(rect.x(), side_y, exclusion.x() - rect.x(),
                           side_height);
  }
  // Handle to the right of the exclusion.
  if (rect.right() > exclusion.right()) {
    new_rects.emplace_back(exclusion.right(), side_y,
                           rect.right() - exclusion.right(), side_height);
  }
  return new_rects;
}

std::vector<gfx::Rect> getHitBoxesForElement(blink::WebElement& element) {
  gfx::Rect visible_rect = element.VisibleBoundsInWidget();
  if (visible_rect.IsEmpty()) {
    return {};
  }

  std::vector<gfx::Rect> rects = element.ClientRectsInWidget();
  for (auto& rect : rects) {
    rect.InclusiveIntersect(visible_rect);
  }
  std::erase_if(rects, [](const gfx::Rect& rect) { return rect.IsEmpty(); });

  // Just ignore some boxes if there are too many.
  rects.resize(std::min(rects.size(), InteractionPointRefiner::kMaxRects));
  return rects;
}

}  // namespace

InteractionPointRefiner::InteractionPointRefiner(const blink::WebNode& node) {
  blink::WebElement element = node.DynamicTo<blink::WebElement>();
  if (!element.IsNull()) {
    hit_boxes_ = getHitBoxesForElement(element);
  }
}
InteractionPointRefiner::InteractionPointRefiner(std::vector<gfx::Rect> rects)
    : hit_boxes_(rects) {}
InteractionPointRefiner::~InteractionPointRefiner() = default;
InteractionPointRefiner::InteractionPointRefiner(InteractionPointRefiner&&) =
    default;
InteractionPointRefiner& InteractionPointRefiner::operator=(
    InteractionPointRefiner&&) = default;

std::optional<gfx::PointF> InteractionPointRefiner::GetPoint() const {
  for (const auto& rect : hit_boxes_) {
    if (!rect.IsEmpty()) {
      return gfx::PointF(rect.CenterPoint());
    }
  }
  return std::nullopt;
}

void InteractionPointRefiner::ExcludeElement(blink::WebElement& element) {
  std::vector<gfx::Rect> exclusions = getHitBoxesForElement(element);
  for (const auto& exclusion : exclusions) {
    AddExclusion(exclusion);
  }
}

void InteractionPointRefiner::AddExclusion(const gfx::Rect& exclusion) {
  std::vector<gfx::Rect> saved_hit_boxes;
  std::vector<gfx::Rect> new_hit_boxes;
  for (auto& rect : hit_boxes_) {
    if (new_hit_boxes.size() > kMaxRects) {
      // Avoid too much churn in the degenerate case. Just ignore some boxes
      // if there are too many.
      break;
    }
    if (!rect.Intersects(exclusion)) {
      // If the exclusion doesn't affect the rect, keep it as-is.
      saved_hit_boxes.emplace_back(std::move(rect));
      continue;
    }
    if (exclusion.Contains(rect)) {
      // Drop the rect if it is completely contained in the exclusion.
      continue;
    }
    std::vector<gfx::Rect> new_boxes = getNewHitBoxesFor(rect, exclusion);
    new_hit_boxes.insert(new_hit_boxes.end(), new_boxes.begin(),
                         new_boxes.end());
  }
  // Put the unmodified rects first so we effectively do a breadth first
  // search.
  saved_hit_boxes.insert(saved_hit_boxes.end(), new_hit_boxes.begin(),
                         new_hit_boxes.end());
  std::swap(saved_hit_boxes, hit_boxes_);
}

std::optional<gfx::PointF> InteractionPointFromWebNode(
    WebWidget* widget,
    const blink::WebNode& node) {
  if (!base::FeatureList::IsEnabled(
          features::kGlicActorIterativeInteractionPointDiscovery)) {
    blink::WebElement element = node.DynamicTo<blink::WebElement>();
    if (element.IsNull()) {
      return std::nullopt;
    }

    gfx::Rect visible_rect = element.VisibleBoundsInWidget();
    if (visible_rect.IsEmpty()) {
      return std::nullopt;
    }

    std::vector<gfx::Rect> rects = element.ClientRectsInWidget();
    for (auto rect : rects) {
      rect.InclusiveIntersect(visible_rect);
      if (!rect.IsEmpty()) {
        return gfx::PointF(rect.CenterPoint());
      }
    }
    return std::nullopt;
  }

  // Iteratively find an acceptable interaction point. Each iteration we
  // perform a hit test for the center of one of our rectangles. If we hit
  // something other than the tree we expect, we exclude the bounds of that
  // element from further checking. This effectively means we find a point
  // to interact with in O(log(N)) time, if it exists.
  InteractionPointRefiner ipr(node);
  // The feature param uses 0 to mean "unbounded", but keep a defensive cap so
  // malformed/degenerate DOM cases cannot spin forever in production.
  const size_t configured_max_iterations =
      features::kGlicActorInterationPointDiscoveryMaxIterations.Get();
  const size_t max_iterations =
      configured_max_iterations == 0 ? 512 : configured_max_iterations;
  size_t iterations = 0;
  std::optional<gfx::PointF> previous_test_point;
  size_t repeated_point_streak = 0;
  std::optional<gfx::PointF> first_point = ipr.GetPoint();
  if (!first_point.has_value()) {
    return std::nullopt;
  }
  DCHECK(widget);
  for (std::optional<gfx::PointF> test_point = ipr.GetPoint();
       test_point.has_value(); test_point = ipr.GetPoint()) {
    if (previous_test_point.has_value() &&
        previous_test_point.value() == test_point.value()) {
      repeated_point_streak++;
      if (repeated_point_streak >= 3) {
        break;
      }
    } else {
      repeated_point_streak = 0;
    }
    previous_test_point = test_point;

    const blink::WebHitTestResult hit_test_result =
        widget->HitTestResultAt(test_point.value());
    blink::WebElement hit_element = hit_test_result.GetElement();

    // The action target from APC is not as granular as the live DOM hit
    // test. Include shadow host element as the hit test would land on
    // those. Also check if the hit element was pulled in via a Web
    // Components slot.
    if (node.ContainsViaFlatTree(&hit_element)) {
      // Found an interaction point.
      return test_point;
    }
    if (++iterations >= max_iterations) {
      break;
    }

    ipr.ExcludeElement(hit_element);
  }
  return first_point;
}

blink::WebNode GetNodeFromId(const content::RenderFrame& local_root_frame,
                             int32_t node_id) {
  const blink::WebLocalFrame* web_frame = local_root_frame.GetWebFrame();
  if (!web_frame) {
    return blink::WebNode();
  }

  // The passed in frame must be a local root.
  CHECK_EQ(web_frame, web_frame->LocalRoot());

  blink::WebNode node = blink::WebNode::FromDomNodeId(node_id);
  // Make sure the node we're getting belongs to a frame under the local root
  // frame.
  if (node.IsNull() || !node.GetDocument() || !node.GetDocument().GetFrame() ||
      node.GetDocument().GetFrame()->LocalRoot() != web_frame) {
    return blink::WebNode();
  }
  return node;
}

bool IsNodeFocused(const content::RenderFrame& frame,
                   const blink::WebNode& node) {
  blink::WebDocument document = frame.GetWebFrame()->GetDocument();
  blink::WebElement currently_focused = document.FocusedElement();
  blink::WebElement element = node.To<blink::WebElement>();
  return element == currently_focused;
}

bool IsPointWithinViewport(const gfx::Point& point,
                           const content::RenderFrame& frame) {
  CHECK(frame.GetWebFrame());
  CHECK_EQ(frame.GetWebFrame(), frame.GetWebFrame()->LocalRoot());
  gfx::Rect viewport(frame.GetWebFrame()->FrameWidget()->VisibleViewportSize());
  return viewport.Contains(point);
}

bool IsPointWithinViewport(const gfx::PointF& point,
                           const content::RenderFrame& frame) {
  return IsPointWithinViewport(gfx::ToFlooredPoint(point), frame);
}

std::string ToDebugString(const mojom::ToolTargetPtr& target) {
  if (target.is_null()) {
    return "target(null)";
  }

  std::stringstream ss;
  ss << "target(";
  if (target->is_coordinate_dip()) {
    ss << "XY[DIP]=" << target->get_coordinate_dip().x() << ","
       << target->get_coordinate_dip().y();
  } else {
    ss << "ID=" << target->get_dom_node_id();
  }
  ss << ")";
  return ss.str();
}

bool IsNodeWithinViewport(const blink::WebNode& node) {
  blink::WebElement element = node.DynamicTo<blink::WebElement>();
  if (element.IsNull()) {
    return false;
  }

  gfx::Rect rect = element.VisibleBoundsInWidget();
  return !rect.IsEmpty();
}

std::string NodeToDebugString(const blink::WebNode& node) {
  if (node.IsNull()) {
    return "";
  }
  if (node.IsTextNode()) {
    // Truncate it to 100 characters, enough for debugging.
    return base::StrCat({"text=", node.NodeValue().Substring(0u, 100u).Utf8()});
  }
  if (node.IsElementNode()) {
    const blink::WebElement element = node.DynamicTo<blink::WebElement>();
    return base::StrCat({element.TagName().Utf8(),
                         " id=", element.GetIdAttribute().Utf8(),
                         " class=", element.GetAttribute("class").Utf8()});
  }
  if (node.IsDocumentNode()) {
    return "document";
  }
  return "";
}

namespace {

std::string CollapseLabel(std::string_view in, size_t max_len) {
  std::string out;
  bool pending_space = false;
  for (char c : in) {
    const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (space) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      if (out.size() + 1 >= max_len) {
        break;
      }
      out.push_back(' ');
      pending_space = false;
    }
    if (out.size() >= max_len) {
      break;
    }
    out.push_back(c);
  }
  return out;
}

std::string Attr(const blink::WebElement& element, const char* name,
                 size_t max_len) {
  return CollapseLabel(element.GetAttribute(blink::WebString::FromUTF8(name))
                           .Utf8(),
                       max_len);
}

bool IsInteractiveElement(const blink::WebElement& element) {
  const std::string tag = base::ToLowerASCII(element.TagName().Utf8());
  if (tag == "a" || tag == "button" || tag == "input" || tag == "textarea" ||
      tag == "select" || tag == "option" || tag == "label" ||
      tag == "summary") {
    return true;
  }
  if (element.IsEditable()) {
    return true;
  }
  const std::string role = base::ToLowerASCII(Attr(element, "role", 32));
  return role == "button" || role == "link" || role == "textbox" ||
         role == "checkbox" || role == "radio" || role == "menuitem" ||
         role == "tab" || role == "switch" || role == "combobox" ||
         role == "searchbox";
}

std::string ElementClickLabel(const blink::WebElement& element) {
  std::string tag = base::ToLowerASCII(element.TagName().Utf8());
  if (tag.empty()) {
    tag = "element";
  }
  std::string label = tag;
  if (tag == "input") {
    const std::string type = Attr(element, "type", 24);
    if (!type.empty()) {
      label += " type=" + type;
    }
  }
  const std::string role = Attr(element, "role", 24);
  if (!role.empty()) {
    label += " role=" + role;
  }
  const std::string id = CollapseLabel(element.GetIdAttribute().Utf8(), 40);
  if (!id.empty()) {
    label += " id=" + id;
  }
  if (tag == "html" || tag == "body") {
    return label;
  }
  std::string name = Attr(element, "aria-label", 60);
  if (name.empty()) {
    name = Attr(element, "placeholder", 60);
  }
  if (name.empty()) {
    name = Attr(element, "alt", 60);
  }
  if (name.empty()) {
    name = Attr(element, "title", 60);
  }
  if (name.empty()) {
    name = Attr(element, "name", 40);
  }
  if (name.empty()) {
    name = CollapseLabel(element.TextContentAbridged(80).Utf8(), 60);
  }
  if (!name.empty()) {
    label += " \"" + name + "\"";
  }
  return label;
}

}  // namespace

std::string DescribeClickedElement(const blink::WebNode& node) {
  if (node.IsNull()) {
    return "(no element)";
  }
  blink::WebNode current = node;
  if (current.IsTextNode()) {
    current = current.ParentOrShadowHostNode();
  }
  if (!current.IsElementNode()) {
    return node.IsDocumentNode() ? "document" : "(no element)";
  }
  const blink::WebElement element = current.DynamicTo<blink::WebElement>();
  if (element.IsNull()) {
    return "(no element)";
  }
  std::string label = ElementClickLabel(element);
  if (!IsInteractiveElement(element)) {
    blink::WebNode parent = element.ParentOrShadowHostNode();
    for (int i = 0; i < 5 && !parent.IsNull(); ++i) {
      if (parent.IsElementNode()) {
        const blink::WebElement ancestor =
            parent.DynamicTo<blink::WebElement>();
        if (!ancestor.IsNull() && IsInteractiveElement(ancestor)) {
          label += " inside " + ElementClickLabel(ancestor);
          break;
        }
      }
      parent = parent.ParentOrShadowHostNode();
    }
  }
  return label;
}

int ToBlinkInputModifiers(uint32_t mojom_modifiers) {
  int out = blink::WebInputEvent::kNoModifiers;
  if (mojom_modifiers & mojom::kInputModifierAlt) {
    out |= blink::WebInputEvent::kAltKey;
  }
  if (mojom_modifiers & mojom::kInputModifierControl) {
    out |= blink::WebInputEvent::kControlKey;
  }
  if (mojom_modifiers & mojom::kInputModifierMeta) {
    out |= blink::WebInputEvent::kMetaKey;
  }
  if (mojom_modifiers & mojom::kInputModifierShift) {
    out |= blink::WebInputEvent::kShiftKey;
  }
  return out;
}

}  // namespace actor
