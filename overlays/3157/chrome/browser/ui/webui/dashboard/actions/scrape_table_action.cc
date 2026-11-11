#include "chrome/browser/ui/webui/dashboard/actions/scrape_table_action.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_locator_matches.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace {

constexpr char kNthChildToken[] = ":nth-child(";
constexpr char kNthOfTypeToken[] = ":nth-of-type(";

std::optional<int> ParsePositiveIntFromToken(std::string value) {
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  if (value.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  if (!base::StringToInt(value, &parsed) || parsed < 1) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::vector<int>> ParseOneBasedIndexSpec(
    const std::string& raw_spec) {
  std::string spec = raw_spec;
  base::TrimWhitespaceASCII(spec, base::TRIM_ALL, &spec);
  if (spec.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> tokens =
      base::SplitString(spec, ",", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (tokens.empty()) {
    return std::nullopt;
  }

  std::vector<int> parsed_indices;
  std::set<int> seen;
  for (std::string token : tokens) {
    base::TrimWhitespaceASCII(token, base::TRIM_ALL, &token);
    if (token.empty()) {
      return std::nullopt;
    }

    const size_t dash = token.find('-');
    if (dash != std::string::npos) {
      if (token.find('-', dash + 1) != std::string::npos) {
        return std::nullopt;
      }

      std::optional<int> start =
          ParsePositiveIntFromToken(token.substr(0, dash));
      std::optional<int> end =
          ParsePositiveIntFromToken(token.substr(dash + 1));
      if (!start.has_value() || !end.has_value() || *start > *end) {
        return std::nullopt;
      }

      for (int one_based = *start; one_based <= *end; ++one_based) {
        if (seen.insert(one_based).second) {
          parsed_indices.push_back(one_based);
        }
      }
      continue;
    }

    std::optional<int> single = ParsePositiveIntFromToken(token);
    if (!single.has_value()) {
      return std::nullopt;
    }
    if (seen.insert(*single).second) {
      parsed_indices.push_back(*single);
    }
  }

  if (parsed_indices.empty()) {
    return std::nullopt;
  }
  return parsed_indices;
}

struct NthLocatorExpansionSpec {
  size_t locator_index = 0;
  std::vector<int> one_based_indices;
  size_t open_paren = 0;
  size_t close_paren = 0;
};

std::optional<NthLocatorExpansionSpec> ParseNthLocatorExpansionSpec(
    const dashboard::Locator& locator,
    size_t locator_index) {
  if (locator.type != dashboard::LocatorType::CSS_SELECTOR) {
    return std::nullopt;
  }
  const std::string selector = locator.value;
  if (selector.empty()) {
    return std::nullopt;
  }

  const std::string selector_lower = base::ToLowerASCII(selector);
  size_t token_pos = selector_lower.find(kNthChildToken);
  size_t token_len = sizeof(kNthChildToken) - 1;
  if (token_pos == std::string::npos) {
    token_pos = selector_lower.find(kNthOfTypeToken);
    token_len = sizeof(kNthOfTypeToken) - 1;
  }
  if (token_pos == std::string::npos) {
    return std::nullopt;
  }

  const size_t open_paren = token_pos + token_len - 1;
  const size_t spec_start = open_paren + 1;
  const size_t close_paren = selector_lower.find(')', spec_start);
  if (close_paren == std::string::npos || close_paren <= spec_start) {
    return std::nullopt;
  }

  std::optional<std::vector<int>> parsed = ParseOneBasedIndexSpec(
      selector.substr(spec_start, close_paren - spec_start));
  if (!parsed.has_value() || parsed->size() <= 1) {
    return std::nullopt;
  }

  NthLocatorExpansionSpec out;
  out.locator_index = locator_index;
  out.one_based_indices = std::move(*parsed);
  out.open_paren = open_paren;
  out.close_paren = close_paren;
  return out;
}

std::string ReplaceNthLocatorIndex(const std::string& selector,
                                   const NthLocatorExpansionSpec& expansion,
                                   int one_based_index) {
  return selector.substr(0, expansion.open_paren + 1) +
         base::NumberToString(one_based_index) +
         selector.substr(expansion.close_paren);
}

std::string BuildExpandedFieldName(const std::string& base_name,
                                   int one_based_index) {
  if (base_name.empty()) {
    return "column_" + base::NumberToString(one_based_index);
  }
  std::string expanded = base_name;
  const std::string token = "{n}";
  size_t pos = expanded.find(token);
  if (pos != std::string::npos) {
    const std::string replacement = base::NumberToString(one_based_index);
    while (pos != std::string::npos) {
      expanded.replace(pos, token.size(), replacement);
      pos = expanded.find(token, pos + replacement.size());
    }
    return expanded;
  }
  return base_name + "_" + base::NumberToString(one_based_index);
}

std::optional<int> ParseNthSingleIndexFromCssSelector(
    const std::string& selector) {
  if (selector.empty()) {
    return std::nullopt;
  }
  const std::string selector_lower = base::ToLowerASCII(selector);
  size_t token_pos = selector_lower.find(kNthChildToken);
  size_t token_len = sizeof(kNthChildToken) - 1;
  if (token_pos == std::string::npos) {
    token_pos = selector_lower.find(kNthOfTypeToken);
    token_len = sizeof(kNthOfTypeToken) - 1;
  }
  if (token_pos == std::string::npos) {
    return std::nullopt;
  }
  const size_t spec_start = token_pos + token_len;
  const size_t close_paren = selector_lower.find(')', spec_start);
  if (close_paren == std::string::npos || close_paren <= spec_start) {
    return std::nullopt;
  }
  std::optional<std::vector<int>> parsed = ParseOneBasedIndexSpec(
      selector.substr(spec_start, close_paren - spec_start));
  if (!parsed.has_value() || parsed->size() != 1) {
    return std::nullopt;
  }
  return parsed->front();
}

std::optional<int> ParseFieldNthSingleIndexOneBased(
    const dashboard::ScrapeTableField& field) {
  for (const auto& locator : field.locators) {
    if (locator.type != dashboard::LocatorType::CSS_SELECTOR) {
      continue;
    }
    if (std::optional<int> parsed =
            ParseNthSingleIndexFromCssSelector(locator.value);
        parsed.has_value()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::string HeaderTextToFieldName(std::string text) {
  text = base::ToLowerASCII(
      base::CollapseWhitespaceASCII(std::move(text), true));
  if (text.empty()) {
    return std::string();
  }
  std::string out;
  out.reserve(text.size());
  bool previous_underscore = false;
  for (char ch : text) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      out.push_back(ch);
      previous_underscore = false;
      continue;
    }
    if (!previous_underscore) {
      out.push_back('_');
      previous_underscore = true;
    }
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::optional<std::pair<std::string, int>> ParseTrailingNumericNameSuffix(
    std::string name) {
  base::TrimWhitespaceASCII(name, base::TRIM_ALL, &name);
  if (name.empty()) {
    return std::nullopt;
  }
  const size_t underscore = name.rfind('_');
  if (underscore == std::string::npos || underscore == 0 ||
      underscore + 1 >= name.size()) {
    return std::nullopt;
  }
  std::string suffix = name.substr(underscore + 1);
  int parsed_index = 0;
  if (!base::StringToInt(suffix, &parsed_index) || parsed_index < 1) {
    return std::nullopt;
  }
  return std::make_pair(name.substr(0, underscore), parsed_index);
}

bool IsAutoGeneratedFieldName(const std::string& name) {
  std::string trimmed =
      std::string(base::TrimWhitespaceASCII(name, base::TRIM_ALL));
  if (trimmed.empty()) {
    return true;
  }
  return base::StartsWith(base::ToLowerASCII(trimmed), "column_",
                          base::CompareCase::SENSITIVE);
}

bool IsLikelyAutoExpandedFieldName(
    const dashboard::ScrapeTableField& field,
    const std::vector<dashboard::ScrapeTableField>& all_fields) {
  const std::optional<std::pair<std::string, int>> suffix =
      ParseTrailingNumericNameSuffix(field.name);
  if (!suffix.has_value()) {
    return false;
  }
  const std::optional<int> nth_index = ParseFieldNthSingleIndexOneBased(field);
  if (!nth_index.has_value() || *nth_index != suffix->second ||
      suffix->first.empty()) {
    return false;
  }

  int sibling_count = 0;
  for (const auto& candidate : all_fields) {
    const std::optional<std::pair<std::string, int>> candidate_suffix =
        ParseTrailingNumericNameSuffix(candidate.name);
    if (!candidate_suffix.has_value() ||
        candidate_suffix->first != suffix->first) {
      continue;
    }
    const std::optional<int> candidate_nth =
        ParseFieldNthSingleIndexOneBased(candidate);
    if (!candidate_nth.has_value() ||
        *candidate_nth != candidate_suffix->second) {
      continue;
    }
    ++sibling_count;
  }
  return sibling_count >= 2;
}

std::vector<dashboard::ScrapeTableField> ConvertFields(
    const base::Value::List& fields_list) {
  std::vector<dashboard::ScrapeTableField> result;
  for (const auto& field_value : fields_list) {
    const base::Value::Dict* field_dict = field_value.GetIfDict();
    if (!field_dict) {
      continue;
    }

    dashboard::ScrapeTableField field;
    const std::string* name = field_dict->FindString("name");
    const base::Value::List* locators = field_dict->FindList("locators");
    if (!locators || locators->empty()) {
      continue;
    }

    field.name = name ? *name : std::string("column");
    base::TrimWhitespaceASCII(field.name, base::TRIM_ALL, &field.name);
    if (field.name.empty()) {
      field.name = "column";
    }
    field.locators = dashboard::ConvertLocators(*locators);
    if (field.locators.empty()) {
      continue;
    }

    const base::Value::List* rules = field_dict->FindList("findReplace");
    if (!rules) {
      rules = field_dict->FindList("findReplaceRules");
    }
    if (rules) {
      field.find_replace_rules = dashboard::ConvertFindReplace(*rules);
    }

    std::vector<NthLocatorExpansionSpec> range_hits;
    for (size_t locator_index = 0; locator_index < field.locators.size();
         ++locator_index) {
      if (std::optional<NthLocatorExpansionSpec> parsed =
              ParseNthLocatorExpansionSpec(field.locators[locator_index],
                                           locator_index);
          parsed.has_value()) {
        range_hits.push_back(std::move(*parsed));
      }
    }

    if (range_hits.empty()) {
      result.push_back(std::move(field));
      continue;
    }

    const NthLocatorExpansionSpec& expansion = range_hits.front();
    for (int one_based_index : expansion.one_based_indices) {
      dashboard::ScrapeTableField expanded_field = field;
      expanded_field.name =
          BuildExpandedFieldName(field.name, one_based_index);
      expanded_field.locators[expansion.locator_index].value =
          ReplaceNthLocatorIndex(
              expanded_field.locators[expansion.locator_index].value,
              expansion, one_based_index);
      result.push_back(std::move(expanded_field));
    }
  }
  return result;
}

base::FilePath ScrapeDataDir(Profile* profile) {
  return profile->GetPath().AppendASCII("SimulachromeData");
}

base::FilePath ScrapeCsvDir(Profile* profile) {
  return ScrapeDataDir(profile).AppendASCII("scrapes");
}

base::FilePath MakeUniqueCsvPathIfNeeded(const base::FilePath& requested_path) {
  if (!base::PathExists(requested_path)) {
    return requested_path;
  }
  for (int suffix = 2; suffix < 10000; ++suffix) {
    const base::FilePath candidate =
        requested_path.InsertBeforeExtensionASCII(
            "_" + base::NumberToString(suffix));
    if (!base::PathExists(candidate)) {
      return candidate;
    }
  }
  return requested_path.InsertBeforeExtensionASCII("_copy");
}

std::string FallbackCsvFileNameFromVariable(const std::string& variable_name) {
  std::string base_name = variable_name;
  base::TrimWhitespaceASCII(base_name, base::TRIM_ALL, &base_name);
  if (base_name.empty()) {
    base_name = "scrape_table_rows";
  }
  return base_name + ".csv";
}

std::string SanitizeCsvFileName(std::string candidate,
                                const std::string& variable_name) {
  base::TrimWhitespaceASCII(candidate, base::TRIM_ALL, &candidate);
  if (candidate.empty()) {
    candidate = FallbackCsvFileNameFromVariable(variable_name);
  }

  for (char& ch : candidate) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    const bool allow =
        std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.' || ch == ' ';
    if (!allow) {
      ch = '_';
    }
  }

  if (!base::EndsWith(candidate, ".csv",
                      base::CompareCase::INSENSITIVE_ASCII)) {
    candidate += ".csv";
  }

  if (candidate == ".csv") {
    return FallbackCsvFileNameFromVariable(variable_name);
  }
  return candidate;
}

std::string EscapeCsvCell(const std::string& value) {
  bool needs_quote = false;
  for (char ch : value) {
    if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
      needs_quote = true;
      break;
    }
  }
  if (!needs_quote) {
    return value;
  }

  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

std::string BuildCsvContent(const std::vector<dashboard::ScrapeTableField>& fields,
                            const base::Value::List& rows) {
  std::string out;

  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    out.append(EscapeCsvCell(fields[i].name));
  }
  out.push_back('\n');

  for (const auto& row_value : rows) {
    const base::Value::Dict* row_dict = row_value.GetIfDict();
    if (!row_dict) {
      continue;
    }

    for (size_t i = 0; i < fields.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      const std::string* raw = row_dict->FindString(fields[i].name);
      out.append(EscapeCsvCell(raw ? *raw : std::string()));
    }
    out.push_back('\n');
  }

  return out;
}

std::pair<bool, base::FilePath> WriteCsvBlocking(
    base::FilePath requested_csv_path,
    std::string csv_content,
    bool append,
    bool make_unique_if_needed) {
  const auto extract_header_line = [](const std::string& content)
      -> std::optional<std::string> {
    const size_t first_newline = content.find('\n');
    if (first_newline == std::string::npos) {
      return std::nullopt;
    }
    return content.substr(0, first_newline + 1);
  };
  const auto append_state_path_for = [](const base::FilePath& csv_path) {
    return csv_path.AddExtensionASCII("dashboard_append_state");
  };
  const auto write_append_state = [](const base::FilePath& state_path,
                                     const std::string& header_line) {
    if (header_line.empty()) {
      return false;
    }
    return base::WriteFile(state_path, header_line);
  };
  const auto read_append_state = [](const base::FilePath& state_path)
      -> std::optional<std::string> {
    std::string header_line;
    if (!base::ReadFileToString(state_path, &header_line) ||
        header_line.empty()) {
      return std::nullopt;
    }
    return header_line;
  };

  base::FilePath csv_path = std::move(requested_csv_path);
  if (make_unique_if_needed) {
    csv_path = MakeUniqueCsvPathIfNeeded(csv_path);
  }

  if (!base::CreateDirectory(csv_path.DirName())) {
    return {false, csv_path};
  }

  const std::optional<std::string> header_line = extract_header_line(csv_content);
  const base::FilePath append_state_path = append_state_path_for(csv_path);

  if (append && base::PathExists(csv_path)) {
    const size_t first_newline = csv_content.find('\n');
    if (first_newline == std::string::npos ||
        first_newline + 1 >= csv_content.size()) {
      return {true, csv_path};
    }

    const std::string data_content = csv_content.substr(first_newline + 1);
    if (data_content.empty()) {
      return {true, csv_path};
    }

    bool should_repeat_header = true;
    if (header_line.has_value()) {
      if (std::optional<std::string> previous_header =
              read_append_state(append_state_path);
          previous_header.has_value() &&
          *previous_header == *header_line) {
        should_repeat_header = false;
      }
    }

    const std::string content_to_append =
        should_repeat_header ? csv_content : data_content;
    if (!base::AppendToFile(csv_path, content_to_append)) {
      return {false, csv_path};
    }
    if (header_line.has_value()) {
      write_append_state(append_state_path, *header_line);
    }
    return {true, csv_path};
  }

  const int rv = base::WriteFile(csv_path, base::as_byte_span(csv_content));
  if (rv >= 0 && header_line.has_value()) {
    write_append_state(append_state_path, *header_line);
  }
  return {rv >= 0, csv_path};
}

std::string NormalizeCellValue(std::string value) {
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  return value;
}

std::string BestCandidateValue(const dashboard::NativePageMapCandidate& candidate) {
  if (!candidate.text.empty()) {
    return NormalizeCellValue(candidate.text);
  }
  if (!candidate.accessibility_name.empty()) {
    return NormalizeCellValue(candidate.accessibility_name);
  }
  if (!candidate.name_attr.empty()) {
    return NormalizeCellValue(candidate.name_attr);
  }
  if (!candidate.id_attr.empty()) {
    return NormalizeCellValue(candidate.id_attr);
  }
  return std::string();
}

std::string NormalizeLocatorHint(std::string value) {
  value = base::CollapseWhitespaceASCII(std::move(value), true);
  return base::ToLowerASCII(NormalizeCellValue(std::move(value)));
}

bool CandidateIsRootLike(const dashboard::NativePageMapCandidate& candidate) {
  const std::string tag = base::ToLowerASCII(candidate.tag);
  const std::string css = base::ToLowerASCII(candidate.css_selector);
  const std::string xpath = base::ToLowerASCII(candidate.xpath);
  return tag == "body" || tag == "html" || css == "body" || css == "html" ||
         xpath == "//body" || xpath == "//html" || xpath == "/html/body" ||
         xpath == "/html";
}

bool CandidateIsLikelyNoiseValue(
    const dashboard::NativePageMapCandidate& candidate) {
  const std::string tag = base::ToLowerASCII(candidate.tag);
  if (tag == "html" || tag == "body" || tag == "head" || tag == "script" ||
      tag == "style" || tag == "noscript" || tag == "meta" ||
      tag == "link" || tag == "svg" || tag == "path" || tag == "defs" ||
      tag == "filter") {
    return true;
  }

  const std::string value = BestCandidateValue(candidate);
  if (value.empty()) {
    return true;
  }
  if (value.size() > 400) {
    return true;
  }

  const std::string lowered = base::ToLowerASCII(value);
  return lowered.find("sourcemappingurl=") != std::string::npos ||
         lowered.find("googletagmanager.com") != std::string::npos ||
         lowered.find("beacon-v2.helpscout.net") != std::string::npos ||
         lowered.find("function (w, d, s, l, i)") != std::string::npos ||
         lowered.find("@keyframes") != std::string::npos;
}

bool CandidateMatchesExactFieldHint(
    const dashboard::ScrapeTableField& field,
    const dashboard::NativePageMapCandidate& candidate) {
  const std::string value_hint = NormalizeLocatorHint(BestCandidateValue(candidate));
  const std::string text_hint = NormalizeLocatorHint(candidate.text);
  const std::string ax_hint =
      NormalizeLocatorHint(candidate.accessibility_name);
  const std::string id_hint = NormalizeLocatorHint(candidate.id_attr);
  const std::string class_hint = NormalizeLocatorHint(candidate.class_attr);
  const std::string tag_hint = NormalizeLocatorHint(candidate.tag);
  const std::string css_hint = NormalizeLocatorHint(candidate.css_selector);
  const std::string xpath_hint = NormalizeLocatorHint(candidate.xpath);

  for (const auto& locator : field.locators) {
    const std::string expected = NormalizeLocatorHint(locator.value);
    if (expected.empty()) {
      continue;
    }
    switch (locator.type) {
      case dashboard::LocatorType::ACCESSIBILITY_NAME:
        if (ax_hint == expected || value_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::TEXT_CONTENT:
      case dashboard::LocatorType::LINK_TEXT:
      case dashboard::LocatorType::PARTIAL_LINK_TEXT:
        if (text_hint == expected || value_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::ID:
        if (id_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::CLASS_NAME:
        if (class_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::TAG_NAME:
        if (tag_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::CSS_SELECTOR:
        if (css_hint == expected) {
          return true;
        }
        break;
      case dashboard::LocatorType::XPATH:
        if (xpath_hint == expected) {
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

bool CandidateContainsFieldHint(const dashboard::ScrapeTableField& field,
                                const dashboard::NativePageMapCandidate& candidate) {
  const std::string value_hint = NormalizeLocatorHint(BestCandidateValue(candidate));
  const std::string text_hint = NormalizeLocatorHint(candidate.text);
  const std::string ax_hint =
      NormalizeLocatorHint(candidate.accessibility_name);
  for (const auto& locator : field.locators) {
    if (locator.type != dashboard::LocatorType::ACCESSIBILITY_NAME &&
        locator.type != dashboard::LocatorType::TEXT_CONTENT &&
        locator.type != dashboard::LocatorType::LINK_TEXT &&
        locator.type != dashboard::LocatorType::PARTIAL_LINK_TEXT) {
      continue;
    }
    const std::string expected = NormalizeLocatorHint(locator.value);
    if (expected.empty()) {
      continue;
    }
    if (value_hint.find(expected) != std::string::npos ||
        text_hint.find(expected) != std::string::npos ||
        ax_hint.find(expected) != std::string::npos) {
      return true;
    }
  }
  return false;
}

int GuidedSingleRecordFieldScore(
    const dashboard::ScrapeTableField& field,
    const dashboard::NativePageMapCandidate& candidate) {
  if (CandidateIsRootLike(candidate) || CandidateIsLikelyNoiseValue(candidate)) {
    return std::numeric_limits<int>::min() / 4;
  }

  const std::string value = BestCandidateValue(candidate);
  if (value.empty()) {
    return std::numeric_limits<int>::min() / 4;
  }

  int score = 0;
  const std::string tag = base::ToLowerASCII(candidate.tag);
  if (CandidateMatchesExactFieldHint(field, candidate)) {
    score += 450;
  } else if (CandidateContainsFieldHint(field, candidate)) {
    score += 120;
  }
  if (candidate.visible) {
    score += 120;
  }
  if (tag == "td" || tag == "th") {
    score += 180;
  } else if (tag == "a" || tag == "button" || tag == "label") {
    score += 120;
  } else if (tag == "span" || tag == "p" || tag == "li" || tag == "h1" ||
             tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
             tag == "h6") {
    score += 80;
  } else if (tag == "div") {
    score += 20;
  }
  if (candidate.clickable || candidate.focusable) {
    score += 25;
  }

  const size_t value_length = value.size();
  if (value_length <= 80) {
    score += 70;
  } else if (value_length <= 160) {
    score += 35;
  } else if (value_length > 250) {
    score -= 120;
  }

  const int area = std::max(1, candidate.width * candidate.height);
  score -= std::min(220, area / 1500);
  return score;
}

std::optional<dashboard::NativePageMapCandidate>
BestFieldCandidateForGuidedSingleRecord(
    const dashboard::ScrapeTableField& field,
    const std::vector<dashboard::NativePageMapCandidate>& matches) {
  int best_score = std::numeric_limits<int>::min();
  const dashboard::NativePageMapCandidate* best = nullptr;
  for (const auto& candidate : matches) {
    const int score = GuidedSingleRecordFieldScore(field, candidate);
    if (score > best_score) {
      best_score = score;
      best = &candidate;
    }
  }
  if (!best || best_score <= std::numeric_limits<int>::min() / 8) {
    return std::nullopt;
  }
  return *best;
}

bool HasPathPrefix(const std::string& parent, const std::string& child) {
  if (parent.empty() || child.empty()) {
    return false;
  }
  if (child == parent) {
    return true;
  }
  return base::StartsWith(child, parent + "/",
                          base::CompareCase::SENSITIVE);
}

std::string ParentPath(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return std::string();
  }
  return path.substr(0, slash);
}

bool SharesParentPath(const dashboard::NativePageMapCandidate& a,
                      const dashboard::NativePageMapCandidate& b) {
  if (a.resource_path.empty() || b.resource_path.empty()) {
    return false;
  }
  const std::string a_parent = ParentPath(a.resource_path);
  const std::string b_parent = ParentPath(b.resource_path);
  return !a_parent.empty() && a_parent == b_parent;
}

bool CandidateCenterInside(const dashboard::NativePageMapCandidate& outer,
                           const dashboard::NativePageMapCandidate& inner) {
  const int center_x = inner.x + inner.width / 2;
  const int center_y = inner.y + inner.height / 2;
  return center_x >= outer.x && center_x <= outer.x + outer.width &&
         center_y >= outer.y && center_y <= outer.y + outer.height;
}

bool CandidateWithinRow(const dashboard::NativePageMapCandidate& row,
                        const dashboard::NativePageMapCandidate& candidate) {
  if (row.frame_id != candidate.frame_id) {
    return false;
  }
  if (HasPathPrefix(row.resource_path, candidate.resource_path)) {
    return true;
  }
  if (SharesParentPath(row, candidate)) {
    return true;
  }
  return CandidateCenterInside(row, candidate);
}

bool CandidateLooksLikeCell(const dashboard::NativePageMapCandidate& candidate) {
  const std::string tag = base::ToLowerASCII(candidate.tag);
  const std::string role = base::ToLowerASCII(candidate.role);
  if (tag == "td" || tag == "th") {
    return true;
  }
  return role == "cell" || role == "gridcell" || role == "columnheader" ||
         role == "rowheader";
}

bool CandidateIsDirectRowChild(const dashboard::NativePageMapCandidate& row,
                               const dashboard::NativePageMapCandidate& candidate) {
  if (row.frame_id != candidate.frame_id || row.resource_path.empty() ||
      candidate.resource_path.empty()) {
    return false;
  }
  return ParentPath(candidate.resource_path) == row.resource_path;
}

std::vector<dashboard::NativePageMapCandidate> CollectRowCellCandidates(
    const dashboard::NativePageMapCandidate& row,
    const std::vector<dashboard::NativePageMapCandidate>& all_candidates) {
  std::vector<dashboard::NativePageMapCandidate> direct_children;
  std::vector<dashboard::NativePageMapCandidate> cell_like_descendants;
  std::vector<dashboard::NativePageMapCandidate> descendants;

  for (const auto& candidate : all_candidates) {
    if (candidate.resource_path == row.resource_path ||
        !CandidateWithinRow(row, candidate)) {
      continue;
    }
    if (CandidateIsDirectRowChild(row, candidate)) {
      direct_children.push_back(candidate);
    }
    if (CandidateLooksLikeCell(candidate)) {
      cell_like_descendants.push_back(candidate);
    }
    descendants.push_back(candidate);
  }

  auto sorter = [](auto& items) {
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) {
                return std::tie(a.frame_id, a.y, a.x, a.resource_path) <
                       std::tie(b.frame_id, b.y, b.x, b.resource_path);
              });
  };
  sorter(direct_children);
  sorter(cell_like_descendants);
  sorter(descendants);

  if (!cell_like_descendants.empty()) {
    return cell_like_descendants;
  }
  if (!direct_children.empty()) {
    return direct_children;
  }
  return descendants;
}

std::optional<dashboard::NativePageMapCandidate> PickRowCellForVisualColumn(
    const dashboard::NativePageMapCandidate& row,
    const std::vector<dashboard::NativePageMapCandidate>& all_candidates,
    int one_based_column) {
  if (one_based_column < 1) {
    return std::nullopt;
  }

  std::vector<dashboard::NativePageMapCandidate> row_cells =
      CollectRowCellCandidates(row, all_candidates);
  if (row_cells.empty()) {
    return std::nullopt;
  }

  int next_auto_column = 1;
  for (const auto& cell : row_cells) {
    const int start_column =
        cell.col_index > 0 ? cell.col_index : next_auto_column;
    const int span = std::max(1, cell.col_span);
    const int end_column = start_column + span - 1;
    if (one_based_column >= start_column && one_based_column <= end_column) {
      return cell;
    }
    next_auto_column = std::max(next_auto_column, end_column + 1);
  }

  return std::nullopt;
}

int FieldCandidateRowScore(const dashboard::NativePageMapCandidate& row,
                           const dashboard::NativePageMapCandidate& candidate) {
  if (!CandidateWithinRow(row, candidate)) {
    return -1;
  }
  int score = 0;
  if (HasPathPrefix(row.resource_path, candidate.resource_path)) {
    score += 200;
  }
  if (SharesParentPath(row, candidate)) {
    score += 130;
  }
  if (CandidateCenterInside(row, candidate)) {
    score += 90;
  }
  if (candidate.visible) {
    score += 20;
  }
  if (candidate.editable) {
    score += 10;
  }
  if (candidate.focusable) {
    score += 5;
  }
  score -= std::max(0, candidate.y - row.y) / 20;
  score -= std::max(0, candidate.x - row.x) / 40;
  return score;
}

std::optional<dashboard::NativePageMapCandidate> BestFieldCandidateForRow(
    const dashboard::NativePageMapCandidate& row,
    const std::vector<dashboard::NativePageMapCandidate>& matches) {
  int best_score = -1;
  const dashboard::NativePageMapCandidate* best = nullptr;
  for (const auto& candidate : matches) {
    const int score = FieldCandidateRowScore(row, candidate);
    if (score > best_score) {
      best_score = score;
      best = &candidate;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return *best;
}

std::string ApplyFindReplaceRules(
    std::string value,
    const std::vector<dashboard::FindReplace>& rules) {
  for (const auto& rule : rules) {
    base::ReplaceSubstringsAfterOffset(&value, 0, rule.find, rule.replace);
  }
  return value;
}

std::vector<dashboard::NativePageMapCandidate> SortRows(
    std::vector<dashboard::NativePageMapCandidate> rows) {
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) {
              return std::tie(a.frame_id, a.y, a.x, a.resource_path) <
                     std::tie(b.frame_id, b.y, b.x, b.resource_path);
            });
  return rows;
}

int CandidateCenterX(const dashboard::NativePageMapCandidate& candidate) {
  return candidate.x + candidate.width / 2;
}

int CandidateCenterY(const dashboard::NativePageMapCandidate& candidate) {
  return candidate.y + candidate.height / 2;
}

std::optional<dashboard::NativePageMapCandidate> PickBestHeaderCandidate(
    const dashboard::NativePageMapCandidate& row_reference,
    const dashboard::NativePageMapCandidate& field_reference,
    const std::vector<dashboard::NativePageMapCandidate>& header_candidates) {
  const int field_center_x = CandidateCenterX(field_reference);
  const int row_center_y = CandidateCenterY(row_reference);
  int best_score = std::numeric_limits<int>::min();
  const dashboard::NativePageMapCandidate* best = nullptr;
  for (const auto& header : header_candidates) {
    if (header.frame_id != row_reference.frame_id) {
      continue;
    }
    if (header.text.empty() && header.accessibility_name.empty()) {
      continue;
    }
    const int header_center_y = CandidateCenterY(header);
    if (header_center_y >= row_center_y) {
      continue;
    }
    const int dx = std::abs(CandidateCenterX(header) - field_center_x);
    const int dy = row_center_y - header_center_y;
    int score = 1000;
    score -= dx * 3;
    score -= dy;
    if (base::EqualsCaseInsensitiveASCII(header.tag, "th")) {
      score += 120;
    }
    if (SharesParentPath(row_reference, header)) {
      score += 90;
    }
    if (score > best_score) {
      best_score = score;
      best = &header;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return *best;
}

void MaybeDeriveAutoFieldNamesFromHeaders(
    const base::Value::Dict& page_map,
    const std::vector<dashboard::NativePageMapCandidate>& rows,
    const std::vector<std::vector<dashboard::NativePageMapCandidate>>&
        field_matches,
    std::vector<dashboard::ScrapeTableField>* fields) {
  if (!fields || fields->empty() || rows.empty() || field_matches.empty()) {
    return;
  }

  bool has_auto_named = false;
  for (const auto& field : *fields) {
    if (IsAutoGeneratedFieldName(field.name) ||
        IsLikelyAutoExpandedFieldName(field, *fields)) {
      has_auto_named = true;
      break;
    }
  }
  if (!has_auto_named) {
    return;
  }

  dashboard::Locator header_locator;
  header_locator.type = dashboard::LocatorType::TAG_NAME;
  header_locator.value = "th";
  const std::vector<dashboard::NativePageMapCandidate> header_candidates =
      dashboard::FindMatchingPageMapCandidates(
          page_map, std::vector<dashboard::Locator>{header_locator});

  std::set<std::string> used_names_lower;
  for (const auto& field : *fields) {
    if (!IsAutoGeneratedFieldName(field.name) &&
        !IsLikelyAutoExpandedFieldName(field, *fields)) {
      used_names_lower.insert(base::ToLowerASCII(field.name));
    }
  }

  for (size_t field_index = 0; field_index < fields->size(); ++field_index) {
    dashboard::ScrapeTableField& field = (*fields)[field_index];
    const bool is_auto_named =
        IsAutoGeneratedFieldName(field.name) ||
        IsLikelyAutoExpandedFieldName(field, *fields);
    if (!is_auto_named || field_index >= field_matches.size()) {
      continue;
    }

    std::optional<dashboard::NativePageMapCandidate> field_reference =
        BestFieldCandidateForRow(rows.front(), field_matches[field_index]);
    if (!field_reference.has_value()) {
      continue;
    }

    std::string derived;
    std::optional<dashboard::NativePageMapCandidate> header =
        PickBestHeaderCandidate(rows.front(), *field_reference,
                                header_candidates);
    if (header.has_value()) {
      const std::string raw_header = !header->text.empty()
                                         ? header->text
                                         : header->accessibility_name;
      derived = HeaderTextToFieldName(raw_header);
    }
    if (derived.empty()) {
      derived = HeaderTextToFieldName(BestCandidateValue(*field_reference));
    }
    if (derived.empty()) {
      if (std::optional<int> nth_index = ParseFieldNthSingleIndexOneBased(field);
          nth_index.has_value()) {
        derived = "column_" + base::NumberToString(*nth_index);
      }
    }
    if (derived.empty()) {
      continue;
    }

    std::string unique = derived;
    int suffix = 2;
    while (used_names_lower.count(base::ToLowerASCII(unique))) {
      unique = derived + "_" + base::NumberToString(suffix++);
    }
    field.name = unique;
    used_names_lower.insert(base::ToLowerASCII(field.name));
  }
}

std::optional<std::vector<int>> ParseRowIndexSpec(
    const std::string& row_index_spec) {
  std::optional<std::vector<int>> parsed = ParseOneBasedIndexSpec(row_index_spec);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  std::vector<int> zero_based;
  zero_based.reserve(parsed->size());
  for (int one_based : *parsed) {
    zero_based.push_back(one_based - 1);
  }
  return zero_based;
}

bool ApplyRowSelectionIndices(
    bool row_index_mode,
    const std::vector<int>& row_indices_zero_based,
    std::vector<dashboard::NativePageMapCandidate>* rows,
    std::string* error_out);

base::Value::List BuildRowsJsonFromCandidateMatches(
    const std::vector<dashboard::ScrapeTableField>& fields,
    const std::vector<dashboard::NativePageMapCandidate>& rows,
    const std::vector<dashboard::NativePageMapCandidate>& all_candidates,
    const std::vector<std::vector<dashboard::NativePageMapCandidate>>&
        field_matches);

struct ScrapeExtractionAttempt {
  bool success = false;
  bool hard_error = false;
  std::string reason;
  base::Value::List rows_json;
  std::vector<dashboard::ScrapeTableField> output_fields;
};

ScrapeExtractionAttempt ExtractRowsFromPageMap(
    const base::Value::Dict& page_map,
    const std::vector<dashboard::Locator>& row_locators,
    const std::vector<dashboard::ScrapeTableField>& fields,
    bool row_index_mode,
    const std::vector<int>& row_indices_zero_based) {
  ScrapeExtractionAttempt attempt;

  std::vector<dashboard::NativePageMapCandidate> rows;
  if (!row_locators.empty()) {
    rows = SortRows(
        dashboard::FindMatchingPageMapCandidates(page_map, row_locators));
    if (rows.empty()) {
      attempt.reason = "No rows matched row locators in page-map";
      return attempt;
    }
  }

  std::string row_selection_error;
  if (!ApplyRowSelectionIndices(row_index_mode, row_indices_zero_based, &rows,
                                &row_selection_error)) {
    attempt.hard_error = true;
    attempt.reason = std::move(row_selection_error);
    return attempt;
  }

  std::vector<std::vector<dashboard::NativePageMapCandidate>> field_matches;
  field_matches.reserve(fields.size());
  for (const auto& field : fields) {
    field_matches.push_back(
        dashboard::FindMatchingPageMapCandidates(page_map, field.locators));
  }

  const std::vector<dashboard::NativePageMapCandidate> all_candidates =
      dashboard::ExtractPageMapCandidates(page_map);

  attempt.output_fields = fields;
  MaybeDeriveAutoFieldNamesFromHeaders(page_map, rows, field_matches,
                                       &attempt.output_fields);
  attempt.rows_json = BuildRowsJsonFromCandidateMatches(
      attempt.output_fields, rows, all_candidates, field_matches);
  if (attempt.rows_json.empty()) {
    attempt.reason = "No table values extracted from page-map candidates";
    return attempt;
  }

  attempt.success = true;
  return attempt;
}

bool ApplyRowSelectionIndices(
    bool row_index_mode,
    const std::vector<int>& row_indices_zero_based,
    std::vector<dashboard::NativePageMapCandidate>* rows,
    std::string* error_out) {
  if (!row_index_mode) {
    return true;
  }
  if (!rows) {
    if (error_out) {
      *error_out = "Internal scrape_table row selection error";
    }
    return false;
  }

  std::vector<dashboard::NativePageMapCandidate> selected_rows;
  selected_rows.reserve(row_indices_zero_based.size());
  for (int row_index_zero_based : row_indices_zero_based) {
    if (row_index_zero_based < 0 ||
        row_index_zero_based >= static_cast<int>(rows->size())) {
      if (error_out) {
        *error_out = "One or more requested row indices are out of range";
      }
      return false;
    }
    selected_rows.push_back((*rows)[static_cast<size_t>(row_index_zero_based)]);
  }
  *rows = std::move(selected_rows);
  return true;
}

base::Value::List BuildRowsJsonFromCandidateMatches(
    const std::vector<dashboard::ScrapeTableField>& fields,
    const std::vector<dashboard::NativePageMapCandidate>& rows,
    const std::vector<dashboard::NativePageMapCandidate>& all_candidates,
    const std::vector<std::vector<dashboard::NativePageMapCandidate>>&
        field_matches) {
  base::Value::List rows_json;
  const bool guided_single_record_mode =
      rows.size() == 1u && CandidateIsRootLike(rows.front());
  if (rows.empty()) {
    base::Value::Dict row_dict;
    bool has_data = false;
    for (size_t i = 0; i < fields.size(); ++i) {
      std::string value;
      if (i < field_matches.size() && !field_matches[i].empty()) {
        value = BestCandidateValue(field_matches[i].front());
      }
      value =
          ApplyFindReplaceRules(std::move(value), fields[i].find_replace_rules);
      row_dict.Set(fields[i].name, value);
      if (!value.empty()) {
        has_data = true;
      }
    }
    if (has_data) {
      rows_json.Append(std::move(row_dict));
    }
    return rows_json;
  }

  for (const auto& row : rows) {
    base::Value::Dict row_dict;
    bool has_data = false;
    for (size_t i = 0; i < fields.size(); ++i) {
      std::string value;
      if (i < field_matches.size()) {
        std::optional<dashboard::NativePageMapCandidate> field_candidate;
        if (guided_single_record_mode) {
          field_candidate =
              BestFieldCandidateForGuidedSingleRecord(fields[i],
                                                     field_matches[i]);
        } else {
          field_candidate = BestFieldCandidateForRow(row, field_matches[i]);
        }
        if (field_candidate.has_value()) {
          value = BestCandidateValue(*field_candidate);
        } else if (std::optional<int> nth_index =
                       ParseFieldNthSingleIndexOneBased(fields[i]);
                   nth_index.has_value()) {
          if (std::optional<dashboard::NativePageMapCandidate> cell_candidate =
                  PickRowCellForVisualColumn(row, all_candidates, *nth_index)) {
            value = BestCandidateValue(*cell_candidate);
          }
        }
      }
      value =
          ApplyFindReplaceRules(std::move(value), fields[i].find_replace_rules);
      row_dict.Set(fields[i].name, value);
      if (!value.empty()) {
        has_data = true;
      }
    }
    if (has_data) {
      rows_json.Append(std::move(row_dict));
    }
  }
  return rows_json;
}

}  // namespace

ScrapeTableAction::ScrapeTableAction(Profile* profile)
    : dashboard::actions::Action(profile) {}
ScrapeTableAction::~ScrapeTableAction() = default;

bool ScrapeTableAction::SetParams(const base::Value::Dict& params) {
  const std::string* variable_name = params.FindString("variableName");
  const base::Value::List* fields_list = params.FindList("fields");
  if (!fields_list || fields_list->empty()) {
    return false;
  }

  variable_name_ = variable_name ? *variable_name : std::string();
  base::TrimWhitespaceASCII(variable_name_, base::TRIM_ALL, &variable_name_);
  fields_ = ConvertFields(*fields_list);
  if (fields_.empty()) {
    return false;
  }

  if (const base::Value::List* rows = params.FindList("rowLocators")) {
    row_locators_ = dashboard::ConvertLocators(*rows);
  } else if (const base::Value::List* legacy_rows = params.FindList("locators")) {
    row_locators_ = dashboard::ConvertLocators(*legacy_rows);
  } else {
    row_locators_.clear();
  }

  row_index_mode_ = false;
  row_indices_zero_based_.clear();
  if (const std::string* mode = params.FindString("rowSelectionMode")) {
    if (base::EqualsCaseInsensitiveASCII(*mode, "index")) {
      row_index_mode_ = true;
    }
  }
  if (row_index_mode_) {
    const base::Value* row_index_value = params.Find("rowIndex");
    if (!row_index_value) {
      row_indices_zero_based_.push_back(0);
    } else if (row_index_value->is_int()) {
      const int row_index_one_based = std::max(1, row_index_value->GetInt());
      row_indices_zero_based_.push_back(row_index_one_based - 1);
    } else if (row_index_value->is_double()) {
      const int row_index_one_based =
          std::max(1, static_cast<int>(row_index_value->GetDouble()));
      row_indices_zero_based_.push_back(row_index_one_based - 1);
    } else if (row_index_value->is_string()) {
      std::optional<std::vector<int>> parsed =
          ParseRowIndexSpec(row_index_value->GetString());
      if (!parsed.has_value()) {
        return false;
      }
      row_indices_zero_based_ = std::move(*parsed);
    } else {
      return false;
    }
  }

  save_csv_ = params.FindBool("saveCsv").value_or(true);
  append_csv_ = params.FindBool("appendCsv").value_or(false);
  const std::string* csv_file_name = params.FindString("csvFileName");
  std::string csv_name_candidate =
      csv_file_name ? *csv_file_name : std::string();
  std::string csv_name_trimmed = csv_name_candidate;
  base::TrimWhitespaceASCII(csv_name_trimmed, base::TRIM_ALL,
                            &csv_name_trimmed);
  csv_file_name_ = SanitizeCsvFileName(csv_name_candidate, variable_name_);
  const std::string default_csv_file_name =
      SanitizeCsvFileName(std::string(), variable_name_);
  // Treat auto/default names as implicit so repeated non-append runs create
  // unique files instead of silently overwriting the prior scrape artifact.
  csv_file_name_explicit_ =
      !csv_name_trimmed.empty() &&
      !base::EqualsCaseInsensitiveASCII(csv_file_name_,
                                        default_csv_file_name);

  SetSerializedAction("scrape_table", params.Clone());
  return true;
}

void ScrapeTableAction::Execute(content::WebContents* web_contents,
                                Callback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents");
    return;
  }

  resolver_page_map_fallback_attempted_ = false;
  resolver_page_map_extractor_.reset();

  if (!dashboard::ExecutionRunContext::Get().ShouldUseUnifiedPageMap()) {
    StartResolverPageMapFallback(web_contents, std::move(callback),
                                 "page_map_disabled_in_script_mode");
    return;
  }

  dashboard::FetchUnifiedPageMapSnapshot(
      web_contents,
      base::BindOnce(&ScrapeTableAction::OnNativePageMapCaptured,
                     weak_ptr_factory_.GetWeakPtr(), web_contents,
                     std::move(callback)),
      dashboard::mojom::SearchScope::kAll);
}

void ScrapeTableAction::OnNativePageMapCaptured(
    content::WebContents* web_contents,
    Callback callback,
    dashboard::UnifiedPageMapResult result) {
  if (!result.ok) {
    const std::string reason =
        result.error.empty() ? "Native table snapshot failed" : result.error;
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(INFO) << "[ScrapeTable] Unified page-map unavailable (" << reason
                << "), falling back to resolver extraction";
    }
    StartResolverPageMapFallback(web_contents, std::move(callback), reason);
    return;
  }

  ScrapeExtractionAttempt attempt = ExtractRowsFromPageMap(
      result.page_map, row_locators_, fields_, row_index_mode_,
      row_indices_zero_based_);
  if (attempt.success) {
    if (!attempt.output_fields.empty()) {
      fields_ = std::move(attempt.output_fields);
    }
    FinalizeRowsPayload(std::move(callback), std::move(attempt.rows_json),
                        /*used_native_sweep=*/false);
    return;
  }
  if (attempt.hard_error) {
    std::move(callback).Run(false, attempt.reason);
    return;
  }

  if (!resolver_page_map_fallback_attempted_) {
    StartResolverPageMapFallback(web_contents, std::move(callback),
                                 attempt.reason);
    return;
  }

  StartNativeLocatorMatchFallback(web_contents, std::move(callback),
                                  attempt.reason);
}

void ScrapeTableAction::FinalizeRowsPayload(Callback callback,
                                            base::Value::List rows_json,
                                            bool used_native_sweep) {
  std::string final_json_string;
  base::JSONWriter::Write(rows_json, &final_json_string);

  if (!save_csv_) {
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(INFO) << "[ScrapeTable] Returning JSON payload (path="
                << (used_native_sweep ? "native_locator_sweep"
                                           : "page_map_native")
                << ")";
    }
    std::move(callback).Run(true, final_json_string);
    return;
  }

  if (!profile_) {
    std::move(callback).Run(false, "Missing profile for CSV write");
    return;
  }

  const base::FilePath csv_path = ScrapeCsvDir(profile_).AppendASCII(csv_file_name_);
  const std::string csv_content = BuildCsvContent(fields_, rows_json);
  const bool make_unique_if_needed = !append_csv_;

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&WriteCsvBlocking, csv_path, csv_content, append_csv_,
                     make_unique_if_needed),
      base::BindOnce(&ScrapeTableAction::OnCsvWriteFinished,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     final_json_string, make_unique_if_needed, csv_path));
}

void ScrapeTableAction::StartResolverPageMapFallback(
    content::WebContents* web_contents,
    Callback callback,
    const std::string& reason) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents for resolver fallback");
    return;
  }

  resolver_page_map_fallback_attempted_ = true;
  resolver_page_map_extractor_ = std::make_unique<dashboard::PageMapExtractor>();
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ScrapeTable] APC page-map was insufficient (" << reason
              << "), trying native resolver page-map fallback";
  }

  resolver_page_map_extractor_->Run(
      web_contents,
      base::BindOnce(&ScrapeTableAction::OnResolverPageMapFallbackReady,
                     weak_ptr_factory_.GetWeakPtr(), web_contents,
                     std::move(callback), reason),
      /*greedy=*/false,
      /*is_agent_mode=*/false,
      /*timeout_ms=*/4000);
}

void ScrapeTableAction::OnResolverPageMapFallbackReady(
    content::WebContents* web_contents,
    Callback callback,
    const std::string& reason,
    base::Value::Dict resolver_page_map) {
  resolver_page_map_extractor_.reset();

  if (resolver_page_map.empty()) {
    StartNativeLocatorMatchFallback(
        web_contents, std::move(callback),
        "Resolver page-map was empty after APC mismatch: " + reason);
    return;
  }

  ScrapeExtractionAttempt attempt = ExtractRowsFromPageMap(
      resolver_page_map, row_locators_, fields_, row_index_mode_,
      row_indices_zero_based_);
  if (attempt.success) {
    if (!attempt.output_fields.empty()) {
      fields_ = std::move(attempt.output_fields);
    }
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(INFO) << "[ScrapeTable] Resolver page-map fallback succeeded";
    }
    FinalizeRowsPayload(std::move(callback), std::move(attempt.rows_json),
                        /*used_native_sweep=*/false);
    return;
  }
  if (attempt.hard_error) {
    std::move(callback).Run(false, attempt.reason);
    return;
  }

  StartNativeLocatorMatchFallback(
      web_contents, std::move(callback),
      "Resolver page-map also missed table data: " + attempt.reason +
          " (APC reason: " + reason + ")");
}

// Last resort: both page maps skip non-interactive cells, so sweep each frame
// for the row and field locators directly and run the same extraction.
void ScrapeTableAction::StartNativeLocatorMatchFallback(
    content::WebContents* web_contents,
    Callback callback,
    const std::string& reason) {
  std::vector<dashboard::Locator> locators = row_locators_;
  for (const dashboard::ScrapeTableField& field : fields_) {
    locators.insert(locators.end(), field.locators.begin(),
                    field.locators.end());
  }
  if (!web_contents || locators.empty()) {
    std::move(callback).Run(false, reason);
    return;
  }
  dashboard::CollectNativeLocatorMatches(
      web_contents, locators, dashboard::mojom::SearchScope::kAll,
      /*max_per_frame=*/1024,
      base::BindOnce(&ScrapeTableAction::OnNativeLocatorMatchesReady,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     reason));
}

void ScrapeTableAction::OnNativeLocatorMatchesReady(
    Callback callback,
    const std::string& reason,
    base::Value::Dict page_map) {
  ScrapeExtractionAttempt attempt = ExtractRowsFromPageMap(
      page_map, row_locators_, fields_, row_index_mode_,
      row_indices_zero_based_);
  if (!attempt.success) {
    std::move(callback).Run(
        false, attempt.hard_error
                   ? attempt.reason
                   : "Native locator sweep also missed table data: " +
                         attempt.reason + " (" + reason + ")");
    return;
  }
  if (!attempt.output_fields.empty()) {
    fields_ = std::move(attempt.output_fields);
  }
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ScrapeTable] Native locator sweep succeeded";
  }
  FinalizeRowsPayload(std::move(callback), std::move(attempt.rows_json),
                      /*used_native_sweep=*/true);
}

void ScrapeTableAction::OnCsvWriteFinished(Callback callback,
                                           const std::string& payload_json,
                                           bool make_unique_if_needed,
                                           const base::FilePath& requested_csv_path,
                                           std::pair<bool, base::FilePath> write_result) {
  const bool success = write_result.first;
  const base::FilePath& actual_csv_path =
      write_result.second.empty() ? requested_csv_path : write_result.second;
  if (!success) {
    const std::string message = "Failed to persist scrape_table CSV at " +
                                actual_csv_path.AsUTF8Unsafe();
    std::move(callback).Run(false, message);
    return;
  }

  if (dashboard::IsVerboseLoggingEnabled()) {
    if (make_unique_if_needed && actual_csv_path != requested_csv_path) {
      LOG(INFO) << "[ScrapeTable] CSV saved with unique name: "
                << actual_csv_path.AsUTF8Unsafe();
    } else {
      LOG(INFO) << "[ScrapeTable] CSV saved: "
                << actual_csv_path.AsUTF8Unsafe();
    }
  }
  std::move(callback).Run(true, payload_json);
}
