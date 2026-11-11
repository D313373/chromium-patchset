// Copyright 2026
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_agent.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_prewarm.h"
#include "chrome/browser/ui/webui/dashboard/startup_agent_launcher.h"
#include "chrome/browser/ui/webui/dashboard/script_context.h"
#include "chrome/browser/ui/webui/dashboard/script_context_manager.h"
#include "chrome/browser/ui/webui/dashboard/script_executor.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"

namespace dashboard {

namespace {

base::FilePath DataDir(const Profile* profile) {
  return profile->GetPath().AppendASCII("SimulachromeData");
}

base::Value::Dict LoadPersistedGlobalSettingsBlocking(
    Profile* profile) {
  if (!profile) {
    return dashboard::DefaultGlobalSettings();
  }

  const base::FilePath settings_path =
      dashboard::ScriptStore::ScriptsDir(profile).AppendASCII(
          "global_settings.json");
  std::string json;
  if (base::ReadFileToString(settings_path, &json)) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (parsed && parsed->is_dict()) {
      return parsed->GetDict().Clone();
    }
  }
  return dashboard::DefaultGlobalSettings();
}

void ApplyInputBehaviorSettings(const base::Value::Dict& input_behavior) {
  const double jitter =
      input_behavior.FindDouble("clickPathJitterPx").value_or(1.5);
  const double hover_radius =
      input_behavior.FindDouble("clickHoverRadiusPx").value_or(2.0);
  const int hover_moves = input_behavior.FindInt("clickHoverMoves").value_or(3);
  const bool overlay_enabled =
      input_behavior.FindBool("showPlaybackOverlay").value_or(true);
  const bool loop_preview_enabled =
      input_behavior.FindBool("loopPreviewEnabled").value_or(false);

  dashboard::SetClickPathJitterPx(std::clamp(jitter, 0.0, 12.0));
  dashboard::SetClickMicroHoverRadiusPx(std::clamp(hover_radius, 0.0, 24.0));
  dashboard::SetClickMicroHoverMoves(std::clamp(hover_moves, 0, 10));
  dashboard::SetPlaybackOverlayEnabled(overlay_enabled);
  dashboard::SetLoopPreviewEnabled(loop_preview_enabled);
}

base::FilePath VariablesIniPath(const Profile* profile) {
  return DataDir(profile).AppendASCII("variables.ini");
}

base::FilePath SecretsIniPath(const Profile* profile) {
  return DataDir(profile).AppendASCII("secrets.ini");
}

std::string NormalizeDataKey(std::string key) {
  base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
    key = key.substr(1, key.size() - 2);
    base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  }
  return key;
}

base::flat_map<std::string, std::string> ReadKeyValueIniBlocking(
    const base::FilePath& path) {
  base::flat_map<std::string, std::string> out;
  if (!base::PathExists(path))
    return out;
  std::string content;
  if (!base::ReadFileToString(path, &content))
    return out;

  for (const auto& raw :
       base::SplitStringPiece(content, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_ALL)) {
    std::string line(raw);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::string trimmed = line;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    if (trimmed.empty())
      continue;
    if (trimmed[0] == '#' || trimmed[0] == ';')
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = NormalizeDataKey(line.substr(0, eq));
    std::string value = line.substr(eq + 1);
    if (key.empty())
      continue;
    out[key] = value;
  }
  return out;
}

std::string DecodeSecretIniValue(const std::string& encoded_or_plain) {
  std::string cipher;
  if (!base::Base64Decode(encoded_or_plain, &cipher))
    return encoded_or_plain;
  std::string plain;
  if (!OSCrypt::DecryptString(cipher, &plain))
    return encoded_or_plain;
  return plain;
}

struct ExecutionTokenCatalog {
  struct LocatorTokenCandidate {
    std::string value;
    // Empty means plain scalar candidate (legacy behavior).
    std::string source_locator_type;
  };

  // Scalar string replacement map used by one-pass {token} substitution.
  base::flat_map<std::string, std::string> scalar_tokens;
  // Multi-value map used for locator fanout (no comma splitting).
  base::flat_map<std::string, std::vector<LocatorTokenCandidate>>
      locator_value_lists;
};

struct TokenCatalogCacheState {
  bool valid = false;
  base::Time variables_mtime;
  base::Time secrets_mtime;
  ExecutionTokenCatalog catalog;
};

std::map<Profile*, TokenCatalogCacheState>& TokenCatalogCacheByProfile() {
  static base::NoDestructor<std::map<Profile*, TokenCatalogCacheState>> cache;
  return *cache;
}

struct TokenCatalogBuildResult {
  bool reuse_cached = false;
  base::Time variables_mtime;
  base::Time secrets_mtime;
  std::optional<ExecutionTokenCatalog> catalog;
};

base::Time LastModifiedTimeOrZero(const base::FilePath& path) {
  base::File::Info info;
  if (!base::GetFileInfo(path, &info))
    return base::Time();
  return info.last_modified;
}

struct ParsedTaggedKey {
  struct TagRef {
    std::string raw_tag;
    std::string grouped_tag;
    std::optional<int> order;
  };

  std::string clean_key;
  std::string base_key;
  std::vector<TagRef> tags;
};

constexpr size_t kMaxLocatorFanoutPerToken = 256;

std::string CanonicalizeLocatorTypeString(const std::string& raw_type);

struct TagTokenValue {
  std::string value;
  std::optional<int> order;
  size_t sequence = 0;
  bool no_delimiter = false;
};

struct TagLocatorTokenValue {
  ExecutionTokenCatalog::LocatorTokenCandidate candidate;
  std::optional<int> order;
  size_t sequence = 0;
};

ParsedTaggedKey::TagRef ParseTagRef(const std::string& raw_tag) {
  ParsedTaggedKey::TagRef ref;
  ref.raw_tag = raw_tag;
  ref.grouped_tag = raw_tag;
  if (raw_tag.empty())
    return ref;

  size_t suffix_begin = raw_tag.size();
  while (suffix_begin > 0 && base::IsAsciiDigit(raw_tag[suffix_begin - 1]))
    --suffix_begin;
  if (suffix_begin == raw_tag.size() || suffix_begin == 0)
    return ref;

  int parsed_order = 0;
  const std::string suffix = raw_tag.substr(suffix_begin);
  if (!base::StringToInt(suffix, &parsed_order) || parsed_order < 0)
    return ref;
  ref.grouped_tag = raw_tag.substr(0, suffix_begin);
  if (ref.grouped_tag.empty())
    ref.grouped_tag = raw_tag;
  else
    ref.order = parsed_order;
  return ref;
}

TagTokenValue MakeTagTokenValue(const std::string& raw_value,
                                std::optional<int> order,
                                size_t sequence) {
  TagTokenValue out;
  out.value = raw_value;
  out.order = order;
  out.sequence = sequence;
  if (raw_value.size() >= 2) {
    const char first = raw_value.front();
    const char last = raw_value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      out.no_delimiter = true;
      out.value = raw_value.substr(1, raw_value.size() - 2);
    }
  }
  return out;
}

std::vector<TagTokenValue> SortedTagValues(
    const std::vector<TagTokenValue>& values) {
  std::vector<TagTokenValue> out = values;
  std::stable_sort(out.begin(), out.end(),
                   [](const TagTokenValue& a, const TagTokenValue& b) {
                     const bool a_has_order = a.order.has_value();
                     const bool b_has_order = b.order.has_value();
                     if (a_has_order != b_has_order)
                       return a_has_order && !b_has_order;
                     if (a_has_order && b_has_order && a.order != b.order)
                       return a.order.value() < b.order.value();
                     return a.sequence < b.sequence;
                   });
  return out;
}

std::string JoinTagScalarValue(const std::vector<TagTokenValue>& values) {
  if (values.empty())
    return std::string();
  const std::vector<TagTokenValue> sorted = SortedTagValues(values);
  bool use_concat = false;
  std::vector<std::string> parts;
  parts.reserve(sorted.size());
  for (const auto& item : sorted) {
    parts.push_back(item.value);
    use_concat = use_concat || item.no_delimiter;
  }
  return base::JoinString(parts, use_concat ? "" : ",");
}

std::optional<ParsedTaggedKey> ParseTaggedKey(const std::string& raw_key) {
  ParsedTaggedKey out;
  out.clean_key = NormalizeDataKey(raw_key);
  if (out.clean_key.empty())
    return std::nullopt;

  std::vector<std::string> parts = base::SplitString(
      out.clean_key, "#", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty())
    return std::nullopt;

  out.base_key = parts.front();
  if (out.base_key.empty())
    return std::nullopt;

  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string& tag = parts[i];
    if (tag.empty())
      continue;
    const bool already_present = std::any_of(
        out.tags.begin(), out.tags.end(),
        [&tag](const ParsedTaggedKey::TagRef& ref) { return ref.raw_tag == tag; });
    if (!already_present)
      out.tags.push_back(ParseTagRef(tag));
  }
  return out;
}

void SetScalarToken(base::flat_map<std::string, std::string>* tokens,
                    const std::string& key,
                    const std::string& value,
                    bool allow_overwrite) {
  if (!tokens || key.empty())
    return;
  auto it = tokens->find(key);
  if (it == tokens->end()) {
    tokens->emplace(key, value);
    return;
  }
  if (allow_overwrite)
    it->second = value;
}

void SetSingleValueList(
    base::flat_map<std::string,
                   std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>>*
        value_lists,
    const std::string& key,
    std::vector<ExecutionTokenCatalog::LocatorTokenCandidate> values,
    bool allow_overwrite) {
  if (!value_lists || key.empty())
    return;
  auto it = value_lists->find(key);
  if (it == value_lists->end()) {
    value_lists->emplace(key, std::move(values));
    return;
  }
  if (allow_overwrite)
    it->second = std::move(values);
}

void AppendUniqueValue(
    base::flat_map<std::string, std::vector<TagTokenValue>>* out,
    const std::string& key,
    const std::string& value,
    std::optional<int> order,
    size_t sequence) {
  if (!out || key.empty())
    return;
  std::vector<TagTokenValue>& values = (*out)[key];
  const TagTokenValue candidate = MakeTagTokenValue(value, order, sequence);
  const bool duplicate = std::any_of(
      values.begin(), values.end(),
      [&candidate](const TagTokenValue& existing) {
        return existing.value == candidate.value;
      });
  if (!duplicate)
    values.push_back(candidate);
}

void AppendUniqueLocatorTagValue(
    base::flat_map<std::string, std::vector<TagLocatorTokenValue>>* out,
    const std::string& key,
    const ExecutionTokenCatalog::LocatorTokenCandidate& candidate,
    std::optional<int> order,
    size_t sequence) {
  if (!out || key.empty())
    return;
  std::vector<TagLocatorTokenValue>& values = (*out)[key];
  const bool duplicate = std::any_of(
      values.begin(), values.end(),
      [&candidate](const TagLocatorTokenValue& existing) {
        return existing.candidate.value == candidate.value &&
               existing.candidate.source_locator_type ==
                   candidate.source_locator_type;
      });
  if (!duplicate) {
    TagLocatorTokenValue entry;
    entry.candidate = candidate;
    entry.order = order;
    entry.sequence = sequence;
    values.push_back(std::move(entry));
  }
}

std::vector<TagLocatorTokenValue> SortedTagLocatorValues(
    const std::vector<TagLocatorTokenValue>& values) {
  std::vector<TagLocatorTokenValue> out = values;
  std::stable_sort(out.begin(), out.end(),
                   [](const TagLocatorTokenValue& a,
                      const TagLocatorTokenValue& b) {
                     const bool a_has_order = a.order.has_value();
                     const bool b_has_order = b.order.has_value();
                     if (a_has_order != b_has_order)
                       return a_has_order && !b_has_order;
                     if (a_has_order && b_has_order && a.order != b.order)
                       return a.order.value() < b.order.value();
                     return a.sequence < b.sequence;
                   });
  return out;
}

bool AppendLocatorCandidateFromDict(
    const base::Value::Dict& dict,
    std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>* out) {
  if (!out)
    return false;
  const std::string* raw_type = dict.FindString("type");
  const std::string* raw_value = dict.FindString("value");
  if (!raw_type || !raw_value)
    return false;
  const std::string locator_type = CanonicalizeLocatorTypeString(*raw_type);
  if (locator_type.empty())
    return false;
  std::string locator_value = *raw_value;
  base::TrimWhitespaceASCII(locator_value, base::TRIM_ALL, &locator_value);
  if (locator_value.empty())
    return false;
  out->push_back(
      {.value = std::move(locator_value), .source_locator_type = locator_type});
  return true;
}

void CollectLocatorCandidatesFromValue(
    const base::Value& input,
    std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>* out) {
  if (!out)
    return;
  if (const base::Value::Dict* dict = input.GetIfDict()) {
    if (AppendLocatorCandidateFromDict(*dict, out))
      return;
    if (const base::Value::List* locators = dict->FindList("locators")) {
      for (const auto& item : *locators)
        CollectLocatorCandidatesFromValue(item, out);
    }
    return;
  }
  if (const base::Value::List* list = input.GetIfList()) {
    for (const auto& item : *list)
      CollectLocatorCandidatesFromValue(item, out);
  }
}

std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>
BuildLocatorCandidates(const std::string& raw_value) {
  std::vector<ExecutionTokenCatalog::LocatorTokenCandidate> out;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(raw_value, base::JSON_PARSE_RFC);
  if (parsed.has_value())
    CollectLocatorCandidatesFromValue(*parsed, &out);

  if (out.empty()) {
    out.push_back({.value = raw_value, .source_locator_type = std::string()});
  } else {
    base::flat_set<std::string> seen;
    std::vector<ExecutionTokenCatalog::LocatorTokenCandidate> deduped;
    deduped.reserve(out.size());
    for (const auto& candidate : out) {
      const std::string key =
          candidate.source_locator_type + "\x1f" + candidate.value;
      if (seen.insert(key).second)
        deduped.push_back(candidate);
    }
    out = std::move(deduped);
  }
  return out;
}

void AddExecutionTokenEntry(
    ExecutionTokenCatalog* catalog,
    base::flat_map<std::string, std::vector<TagTokenValue>>* tag_values,
    base::flat_map<std::string, std::vector<TagLocatorTokenValue>>*
        locator_tag_values,
    size_t* tag_sequence,
    const std::string& raw_key,
    const std::string& value,
    bool allow_overwrite) {
  if (!catalog || !tag_values || !locator_tag_values || !tag_sequence)
    return;
  std::optional<ParsedTaggedKey> parsed = ParseTaggedKey(raw_key);
  if (!parsed.has_value())
    return;
  const std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>
      locator_candidates = BuildLocatorCandidates(value);

  // Both the raw tagged key and the base key are addressable.
  SetScalarToken(&catalog->scalar_tokens, parsed->clean_key, value,
                 allow_overwrite);
  SetScalarToken(&catalog->scalar_tokens, parsed->base_key, value,
                 allow_overwrite);
  SetSingleValueList(&catalog->locator_value_lists, parsed->clean_key,
                     locator_candidates, allow_overwrite);
  SetSingleValueList(&catalog->locator_value_lists, parsed->base_key,
                     locator_candidates, allow_overwrite);

  for (const ParsedTaggedKey::TagRef& tag : parsed->tags) {
    if (tag.raw_tag.empty())
      continue;
    AppendUniqueValue(tag_values, tag.raw_tag, value, std::nullopt,
                      (*tag_sequence)++);
    for (const auto& locator_candidate : locator_candidates) {
      AppendUniqueLocatorTagValue(locator_tag_values, tag.raw_tag,
                                  locator_candidate, std::nullopt,
                                  (*tag_sequence)++);
    }
    if (!tag.grouped_tag.empty() && tag.grouped_tag != tag.raw_tag) {
      AppendUniqueValue(tag_values, tag.grouped_tag, value, tag.order,
                        (*tag_sequence)++);
      for (const auto& locator_candidate : locator_candidates) {
        AppendUniqueLocatorTagValue(locator_tag_values, tag.grouped_tag,
                                    locator_candidate, tag.order,
                                    (*tag_sequence)++);
      }
    }
  }
}

ExecutionTokenCatalog BuildExecutionTokenCatalog(
    const base::FilePath& variables_path,
    const base::FilePath& secrets_path) {
  ExecutionTokenCatalog catalog;
  size_t tag_sequence = 0;

  base::flat_map<std::string, std::vector<TagTokenValue>> var_tags;
  base::flat_map<std::string, std::vector<TagLocatorTokenValue>>
      var_locator_tags;
  const auto vars = ReadKeyValueIniBlocking(variables_path);
  for (const auto& kv : vars) {
    AddExecutionTokenEntry(&catalog, &var_tags, &var_locator_tags, &tag_sequence,
                           kv.first, kv.second,
                           /*allow_overwrite=*/true);
  }
  for (const auto& [tag, values] : var_tags) {
    catalog.scalar_tokens[tag] = JoinTagScalarValue(values);
  }
  for (const auto& [tag, values] : var_locator_tags) {
    const std::vector<TagLocatorTokenValue> sorted = SortedTagLocatorValues(values);
    std::vector<ExecutionTokenCatalog::LocatorTokenCandidate> list_values;
    list_values.reserve(sorted.size());
    for (const auto& item : sorted)
      list_values.push_back(item.candidate);
    catalog.locator_value_lists[tag] = std::move(list_values);
  }

  // Secrets are additive: fill in missing keys/tags without overriding vars.
  base::flat_map<std::string, std::vector<TagTokenValue>> secret_tags;
  base::flat_map<std::string, std::vector<TagLocatorTokenValue>>
      secret_locator_tags;
  const auto secrets = ReadKeyValueIniBlocking(secrets_path);
  for (const auto& kv : secrets) {
    const std::string plain = DecodeSecretIniValue(kv.second);
    // Never log secret values — only whether punctuation survived decrypt.
    const size_t bang_count =
        static_cast<size_t>(std::count(plain.begin(), plain.end(), '!'));
    const size_t period_count =
        static_cast<size_t>(std::count(plain.begin(), plain.end(), '.'));
    LOG(INFO) << "[Secrets] loaded key=" << kv.first
              << " plain_len=" << plain.size() << " bang_count=" << bang_count
              << " period_count=" << period_count
              << " stored_was_b64ish="
              << (plain != kv.second ? "decoded" : "plaintext_or_failed_decode");
    AddExecutionTokenEntry(&catalog, &secret_tags, &secret_locator_tags,
                           &tag_sequence, kv.first, plain,
                           /*allow_overwrite=*/false);
  }
  for (const auto& [tag, values] : secret_tags) {
    if (!catalog.scalar_tokens.contains(tag)) {
      catalog.scalar_tokens[tag] = JoinTagScalarValue(values);
    }
  }
  for (const auto& [tag, values] : secret_locator_tags) {
    if (!catalog.locator_value_lists.contains(tag)) {
      const std::vector<TagLocatorTokenValue> sorted =
          SortedTagLocatorValues(values);
      std::vector<ExecutionTokenCatalog::LocatorTokenCandidate> list_values;
      list_values.reserve(sorted.size());
      for (const auto& item : sorted)
        list_values.push_back(item.candidate);
      catalog.locator_value_lists[tag] = std::move(list_values);
    }
  }

  return catalog;
}

TokenCatalogBuildResult BuildExecutionTokenCatalogIfNeeded(
    const base::FilePath& variables_path,
    const base::FilePath& secrets_path,
    bool has_cached_catalog,
    base::Time cached_variables_mtime,
    base::Time cached_secrets_mtime) {
  TokenCatalogBuildResult out;
  out.variables_mtime = LastModifiedTimeOrZero(variables_path);
  out.secrets_mtime = LastModifiedTimeOrZero(secrets_path);

  if (has_cached_catalog && out.variables_mtime == cached_variables_mtime &&
      out.secrets_mtime == cached_secrets_mtime) {
    out.reuse_cached = true;
    return out;
  }

  out.catalog =
      BuildExecutionTokenCatalog(variables_path, secrets_path);
  return out;
}

std::string CanonicalizeLocatorTypeString(const std::string& raw_type) {
  std::string t = raw_type;
  base::TrimWhitespaceASCII(t, base::TRIM_ALL, &t);
  if (t.empty())
    return std::string();
  t = base::ToUpperASCII(t);
  std::replace(t.begin(), t.end(), '-', '_');
  std::replace(t.begin(), t.end(), ' ', '_');

  if (t == "CSS" || t == "SELECTOR")
    return "CSS_SELECTOR";
  if (t == "CLASS")
    return "CLASS_NAME";
  if (t == "TAG")
    return "TAG_NAME";
  if (t == "TEXT")
    return "TEXT_CONTENT";
  if (t == "LINK")
    return "LINK_TEXT";
  if (t == "PARTIAL_LINK")
    return "PARTIAL_LINK_TEXT";
  if (t == "A11Y" || t == "ARIA" || t == "ACCESSIBILITY")
    return "ACCESSIBILITY_NAME";
  if (t == "JS" || t == "JAVASCRIPT")
    return "JS_EVAL";
  if (t == "SHADOW")
    return "SHADOW_DOM";
  if (t == "COORDS" || t == "COORDINATES")
    return "VISUAL_COORDS";
  if (t == "FRAME")
    return "FRAME_ID";
  if (t == "RESOURCE")
    return "RESOURCE_TREE";

  static constexpr std::string_view kKnownTypes[] = {
      "ID",
      "CSS_SELECTOR",
      "NAME",
      "ACCESSIBILITY_NAME",
      "CLASS_NAME",
      "JS_EVAL",
      "SHADOW_DOM",
      "LINK_TEXT",
      "PARTIAL_LINK_TEXT",
      "TAG_NAME",
      "XPATH",
      "TEXT_CONTENT",
      "VISUAL_COORDS",
      "VISUAL_AI",
      "FRAME_ID",
      "RESOURCE_TREE",
  };
  for (std::string_view known : kKnownTypes) {
    if (t == known)
      return std::string(known);
  }
  return std::string();
}

bool IsLocatorDict(const base::Value::Dict& dict) {
  const std::string* type = dict.FindString("type");
  const std::string* value = dict.FindString("value");
  return type && value && !CanonicalizeLocatorTypeString(*type).empty();
}

std::optional<std::string> ParseStandaloneTokenReference(
    const std::string& input) {
  std::string trimmed = input;
  base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
  if (trimmed.size() < 3 || trimmed.front() != '{' || trimmed.back() != '}')
    return std::nullopt;
  std::string key = NormalizeDataKey(trimmed.substr(1, trimmed.size() - 2));
  if (key.empty())
    return std::nullopt;
  return key;
}

bool LooksLikeVisualCoords(std::string input) {
  base::TrimWhitespaceASCII(input, base::TRIM_ALL, &input);
  if (input.empty())
    return false;

  std::vector<std::string> parts = base::SplitString(
      input, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.size() == 2) {
    int x = 0;
    int y = 0;
    if (base::StringToInt(parts[0], &x) && base::StringToInt(parts[1], &y))
      return true;
  }

  std::optional<base::Value> parsed =
      base::JSONReader::Read(input, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return false;
  const base::Value::Dict& dict = parsed->GetDict();
  const base::Value* x = dict.Find("x");
  const base::Value* y = dict.Find("y");
  if (!x || !y)
    return false;
  const bool x_ok = x->is_int() || x->is_double();
  const bool y_ok = y->is_int() || y->is_double();
  return x_ok && y_ok;
}

bool IsSimpleTokenValue(const std::string& input) {
  if (input.empty())
    return false;
  for (char ch : input) {
    if (base::IsAsciiWhitespace(ch))
      return false;
  }
  return true;
}

bool IsIdentifierLikeValue(const std::string& input) {
  if (!IsSimpleTokenValue(input))
    return false;
  for (char ch : input) {
    if (!(base::IsAsciiAlpha(ch) || base::IsAsciiDigit(ch) || ch == '_' ||
          ch == '-' || ch == ':' || ch == '.')) {
      return false;
    }
  }
  return true;
}

bool IsTagNameValue(const std::string& input) {
  if (input.empty())
    return false;
  for (char ch : input) {
    if (!(base::IsAsciiAlpha(ch) || base::IsAsciiDigit(ch) || ch == '-' ||
          ch == '_' || ch == ':')) {
      return false;
    }
  }
  return true;
}

bool IsLocatorCandidateCompatible(const std::string& raw_locator_type,
                                  const std::string& raw_value) {
  const std::string type = CanonicalizeLocatorTypeString(raw_locator_type);
  if (type.empty())
    return false;

  std::string value = raw_value;
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  if (value.empty())
    return false;

  if (type == "ID") {
    if (!IsIdentifierLikeValue(value))
      return false;
    return value.front() != '#' && value.front() != '.' &&
           value.front() != '/';
  }
  if (type == "CSS_SELECTOR") {
    if (value.front() == '/' || value.front() == '(')
      return false;
    if (value.find('{') != std::string::npos ||
        value.find('}') != std::string::npos) {
      return false;
    }
    if (LooksLikeVisualCoords(value))
      return false;
    return true;
  }
  if (type == "XPATH") {
    return value.front() == '/' || value.front() == '(';
  }
  if (type == "NAME") {
    if (!IsIdentifierLikeValue(value))
      return false;
    return value.front() != '#' && value.front() != '.' &&
           value.front() != '/';
  }
  if (type == "CLASS_NAME") {
    if (!IsIdentifierLikeValue(value))
      return false;
    return value.front() != '.' && value.front() != '#' &&
           value.front() != '/';
  }
  if (type == "TAG_NAME")
    return IsTagNameValue(value);
  if (type == "VISUAL_COORDS")
    return LooksLikeVisualCoords(value);

  // Remaining locator types accept arbitrary non-empty text.
  return true;
}

bool IsLocatorCandidateCompatible(
    const std::string& raw_locator_type,
    const ExecutionTokenCatalog::LocatorTokenCandidate& candidate) {
  if (!candidate.source_locator_type.empty()) {
    const std::string target_type =
        CanonicalizeLocatorTypeString(raw_locator_type);
    if (target_type.empty() || target_type != candidate.source_locator_type)
      return false;
    std::string value = candidate.value;
    base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
    return !value.empty();
  }
  return IsLocatorCandidateCompatible(raw_locator_type, candidate.value);
}

std::string ExpandTokensInStringOnePass(
    const std::string& input,
    const base::flat_map<std::string, std::string>& tokens,
    int* replacements) {
  if (input.empty() || tokens.empty())
    return input;

  std::string out;
  out.reserve(input.size() + 16);
  size_t cursor = 0;

  while (cursor < input.size()) {
    const size_t open = input.find('{', cursor);
    if (open == std::string::npos)
      break;
    const size_t close = input.find('}', open + 1);
    if (close == std::string::npos)
      break;

    out.append(input, cursor, open - cursor);

    // Support both {Token} and mustache-style {{Token}} (models often emit the
    // latter). Without this, {{Email}} looks up token "{Email" and fails, then
    // the literal is typed — and '{' often drops in native key dispatch.
    size_t name_start = open + 1;
    size_t name_end = close;
    size_t consumed_end = close + 1;
    if (name_start < close && input[name_start] == '{') {
      ++name_start;
      if (close + 1 < input.size() && input[close + 1] == '}')
        consumed_end = close + 2;
    }

    std::string token =
        NormalizeDataKey(input.substr(name_start, name_end - name_start));
    auto it = tokens.find(token);
    if (it == tokens.end()) {
      // Vault keys keep their stored case (Email, Password). The manager
      // often emits {email} / {password}. Use a case-insensitive hit only
      // when exactly one key matches, so {password} cannot land on PasswordS.
      int matches = 0;
      for (auto scan = tokens.begin(); scan != tokens.end(); ++scan) {
        if (!base::EqualsCaseInsensitiveASCII(scan->first, token)) {
          continue;
        }
        it = scan;
        ++matches;
      }
      if (matches != 1) {
        it = tokens.end();
      }
    }
    if (it != tokens.end()) {
      const size_t bang_count = static_cast<size_t>(
          std::count(it->second.begin(), it->second.end(), '!'));
      LOG(INFO) << "[ExpandTokens] token=" << token
                << " matched_key=" << it->first
                << " value_len=" << it->second.size()
                << " bang_count=" << bang_count;
      out.append(it->second);
      if (replacements)
        ++(*replacements);
      cursor = consumed_end;
    } else {
      // Keep the original span unchanged when the token is unknown.
      out.append(input, open, close - open + 1);
      cursor = close + 1;
    }
  }

  out.append(input, cursor, std::string::npos);
  return out;
}

void ExpandTokensInValueOnePass(
    base::Value* value,
    const base::flat_map<std::string, std::string>& tokens,
    int* replacements) {
  if (!value)
    return;
  if (value->is_string()) {
    const std::string expanded =
        ExpandTokensInStringOnePass(value->GetString(), tokens, replacements);
    value->GetString() = expanded;
    return;
  }
  if (value->is_list()) {
    for (auto& v : value->GetList())
      ExpandTokensInValueOnePass(&v, tokens, replacements);
    return;
  }
  if (value->is_dict()) {
    const bool locator_dict = IsLocatorDict(value->GetDict());
    for (auto it = value->GetDict().begin(); it != value->GetDict().end();
         ++it) {
      // Locator values are handled in the dedicated locator expansion pass.
      if (locator_dict && it->first == "value")
        continue;
      ExpandTokensInValueOnePass(&it->second, tokens, replacements);
    }
  }
}

void ExpandLocatorTokensInValue(
    base::Value* value,
    const base::flat_map<std::string, std::string>& scalar_tokens,
    const base::flat_map<
        std::string,
        std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>>& value_lists,
    int* replacements,
    int* locator_fanouts) {
  if (!value)
    return;

  if (value->is_list()) {
    base::Value::List expanded;
    for (auto& item : value->GetList()) {
      if (!item.is_dict()) {
        expanded.Append(std::move(item));
        continue;
      }
      base::Value::Dict& dict = item.GetDict();
      if (!IsLocatorDict(dict)) {
        expanded.Append(std::move(item));
        continue;
      }

      const std::string* type = dict.FindString("type");
      const std::string* raw_value = dict.FindString("value");
      DCHECK(type);
      DCHECK(raw_value);

      std::optional<std::string> maybe_token =
          ParseStandaloneTokenReference(*raw_value);
      if (!maybe_token.has_value()) {
        base::Value::Dict out = dict.Clone();
        const std::string expanded_value =
            ExpandTokensInStringOnePass(*raw_value, scalar_tokens, replacements);
        out.Set("value", expanded_value);
        expanded.Append(std::move(out));
        continue;
      }

      const auto values_it = value_lists.find(*maybe_token);
      if (values_it == value_lists.end()) {
        expanded.Append(std::move(item));
        continue;
      }

      size_t emitted = 0;
      base::flat_set<std::string> emitted_values;
      for (const auto& candidate : values_it->second) {
        if (emitted >= kMaxLocatorFanoutPerToken)
          break;
        if (!IsLocatorCandidateCompatible(*type, candidate))
          continue;
        if (!emitted_values.insert(candidate.value).second)
          continue;
        base::Value::Dict out = dict.Clone();
        out.Set("value", candidate.value);
        expanded.Append(std::move(out));
        ++emitted;
        if (replacements)
          ++(*replacements);
        if (locator_fanouts)
          ++(*locator_fanouts);
      }
      // If no compatible values were emitted, silently drop this locator.
    }
    value->GetList() = std::move(expanded);
    for (auto& child : value->GetList()) {
      ExpandLocatorTokensInValue(&child, scalar_tokens, value_lists,
                                 replacements, locator_fanouts);
    }
    return;
  }

  if (value->is_dict()) {
    for (auto it = value->GetDict().begin(); it != value->GetDict().end();
         ++it) {
      ExpandLocatorTokensInValue(&it->second, scalar_tokens, value_lists,
                                 replacements, locator_fanouts);
    }
  }
}

base::Value::List ExpandTokensInTimelineOnePass(
    const base::Value::List& timeline,
    const base::flat_map<std::string, std::string>& scalar_tokens,
    const base::flat_map<
        std::string,
        std::vector<ExecutionTokenCatalog::LocatorTokenCandidate>>& value_lists,
    int* replacements,
    int* locator_fanouts) {
  base::Value::List out = timeline.Clone();
  for (auto& item : out) {
    ExpandLocatorTokensInValue(&item, scalar_tokens, value_lists, replacements,
                               locator_fanouts);
    ExpandTokensInValueOnePass(&item, scalar_tokens, replacements);
  }
  return out;
}

}  // namespace

ScriptExecutionService::RunState::RunState() = default;
ScriptExecutionService::RunState::~RunState() = default;
ScriptExecutionService::RunState::RunState(RunState&&) = default;
ScriptExecutionService::RunState& ScriptExecutionService::RunState::operator=(
    RunState&&) = default;

ScriptExecutionService::ScriptExecutionService(Profile* profile)
    : profile_(profile) {
  // Scaffolding expands {Token} on the UI thread from this cache. Playback
  // still rebuilds it when the ini files change.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&BuildExecutionTokenCatalogIfNeeded,
                     VariablesIniPath(profile_), SecretsIniPath(profile_),
                     /*has_cached_catalog=*/false, base::Time(), base::Time()),
      base::BindOnce(
          [](base::WeakPtr<ScriptExecutionService> self,
             TokenCatalogBuildResult result) {
            if (!self || !result.catalog.has_value()) {
              return;
            }
            TokenCatalogCacheState& state =
                TokenCatalogCacheByProfile()[self->profile_];
            state.valid = true;
            state.variables_mtime = result.variables_mtime;
            state.secrets_mtime = result.secrets_mtime;
            state.catalog = std::move(*result.catalog);
            LOG(INFO) << "[ExpandTokens] catalog ready keys="
                      << state.catalog.scalar_tokens.size();
          },
          weak_ptr_factory_.GetWeakPtr()));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LoadPersistedGlobalSettingsBlocking, profile_),
      base::BindOnce(
          &ScriptExecutionService::OnPersistedInputBehaviorSettingsLoaded,
          weak_ptr_factory_.GetWeakPtr()));
}

ScriptExecutionService::~ScriptExecutionService() {
  TokenCatalogCacheByProfile().erase(profile_);
}

// static
void ScriptExecutionService::InvalidateTokenCatalogCacheForProfile(
    Profile* profile) {
  if (!profile) {
    return;
  }
  TokenCatalogCacheByProfile().erase(profile);
}

std::string ScriptExecutionService::ExpandExecutionTokens(
    Profile* profile,
    const std::string& input) {
  if (!profile || input.find('{') == std::string::npos) {
    return input;
  }
  auto cache_it = TokenCatalogCacheByProfile().find(profile);
  if (cache_it == TokenCatalogCacheByProfile().end() ||
      !cache_it->second.valid) {
    LOG(WARNING) << "[ExpandTokens] catalog not ready; leaving tokens literal";
    return input;
  }
  int replacements = 0;
  return ExpandTokensInStringOnePass(input, cache_it->second.catalog.scalar_tokens,
                                     &replacements);
}

void ScriptExecutionService::OnPersistedInputBehaviorSettingsLoaded(
    base::Value::Dict settings) {
  dashboard::GlobalSettingsStore() = settings.Clone();
  const base::Value::Dict* input_behavior = settings.FindDict("inputBehavior");
  const base::Value::Dict empty_input_behavior;
  ApplyInputBehaviorSettings(input_behavior ? *input_behavior
                                            : empty_input_behavior);
  dashboard::MaybePrewarmHeadlessListenerHost(profile_);
  dashboard::MaybeScheduleStartupAgentTask(profile_);
  dashboard::HeadlessListenerAgent::GetInstance().MaybeStartForProfile(
      profile_);
}

std::string ScriptExecutionService::StartScriptRun(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const base::Value::List* key_timeline) {
  return StartScriptRunWithOptions(script_id, timeline, target_web_contents,
                                   key_timeline, RunOptions());
}

std::string ScriptExecutionService::StartScriptRunWithOptions(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const base::Value::List* key_timeline,
    const RunOptions& options) {
  return StartRunInternal(script_id, /*persist=*/true, timeline,
                          target_web_contents, key_timeline,
                          /*start_step_index=*/-1, /*resume_url=*/std::string(),
                          options, CompletionCallback());
}

std::string ScriptExecutionService::StartScriptRun(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const base::Value::List* key_timeline,
    CompletionCallback completion) {
  return StartScriptRunWithOptions(script_id, timeline, target_web_contents,
                                   key_timeline, RunOptions(),
                                   std::move(completion));
}

std::string ScriptExecutionService::StartScriptRunWithOptions(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const base::Value::List* key_timeline,
    const RunOptions& options,
    CompletionCallback completion) {
  return StartRunInternal(script_id, /*persist=*/true, timeline,
                          target_web_contents, key_timeline,
                          /*start_step_index=*/-1, /*resume_url=*/std::string(),
                          options, std::move(completion));
}

std::string ScriptExecutionService::StartScriptRunFromStep(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    int start_step_index,
    const std::string& resume_url) {
  return StartRunInternal(script_id, /*persist=*/true, timeline,
                          target_web_contents, /*key_timeline=*/nullptr,
                          start_step_index, resume_url, RunOptions(),
                          CompletionCallback());
}

std::string ScriptExecutionService::StartScriptRunFromStep(
    const std::string& script_id,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    int start_step_index,
    const std::string& resume_url,
    CompletionCallback completion) {
  return StartRunInternal(script_id, /*persist=*/true, timeline,
                          target_web_contents, /*key_timeline=*/nullptr,
                          start_step_index, resume_url, RunOptions(),
                          std::move(completion));
}

std::string ScriptExecutionService::StartAdHocRun(
    const base::Value::List& timeline,
    content::WebContents* target_web_contents) {
  return StartAdHocRunWithOptions(timeline, target_web_contents, RunOptions());
}

std::string ScriptExecutionService::StartAdHocRunWithOptions(
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const RunOptions& options) {
  return StartRunInternal(/*script_id=*/std::string(), /*persist=*/false,
                          timeline, target_web_contents,
                          /*key_timeline=*/nullptr, /*start_step_index=*/-1,
                          /*resume_url=*/std::string(), options,
                          CompletionCallback());
}

std::string ScriptExecutionService::StartAdHocRun(
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    CompletionCallback completion) {
  return StartAdHocRunWithOptions(timeline, target_web_contents, RunOptions(),
                                  std::move(completion));
}

std::string ScriptExecutionService::StartAdHocRunWithOptions(
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const RunOptions& options,
    CompletionCallback completion) {
  return StartRunInternal(/*script_id=*/std::string(), /*persist=*/false,
                          timeline, target_web_contents,
                          /*key_timeline=*/nullptr, /*start_step_index=*/-1,
                          /*resume_url=*/std::string(), options,
                          std::move(completion));
}

bool ScriptExecutionService::StopRun(const std::string& execution_id,
                                     const std::string& reason) {
  auto it = runs_.find(execution_id);
  if (it == runs_.end()) {
    return false;
  }
  if (it->second.executor) {
    LOG(INFO) << "[ExecutionService] StopRun id=" << execution_id
              << " reason=" << reason;
    it->second.executor->StopExecution();
  }
  return true;
}

size_t ScriptExecutionService::StopAllRuns(const std::string& reason) {
  std::vector<std::string> ids;
  ids.reserve(runs_.size());
  for (const auto& [id, state] : runs_) {
    if (state.executor) {
      ids.push_back(id);
    }
  }
  size_t stopped = 0;
  for (const auto& id : ids) {
    if (StopRun(id, reason)) {
      ++stopped;
    }
  }
  if (stopped > 0) {
    LOG(INFO) << "[ExecutionService] StopAllRuns count=" << stopped
              << " reason=" << reason;
  }
  return stopped;
}

base::Value::Dict ScriptExecutionService::GetRunStatusDict(
    const std::string& execution_id) const {
  base::Value::Dict out;
  auto it = runs_.find(execution_id);
  if (it == runs_.end() || !it->second.executor) {
    out.Set("ok", false);
    return out;
  }
  const auto st = it->second.executor->GetStatus();
  out.Set("ok", true);
  out.Set("is_running", st.is_running);
  out.Set("current_step", st.current_step);
  out.Set("total_steps", st.total_steps);
  out.Set("error", st.error);
  out.Set("status_message", st.status_message);
  return out;
}

std::string ScriptExecutionService::StartRunInternal(
    const std::string& script_id,
    bool persist,
    const base::Value::List& timeline,
    content::WebContents* target_web_contents,
    const base::Value::List* key_timeline,
    int start_step_index,
    const std::string& resume_url,
    const RunOptions& options,
    CompletionCallback completion) {
  if (!target_web_contents) {
    LOG(ERROR) << "[ExecutionService] StartRun: no target WebContents";
    return std::string();
  }

  const int target_tab_id =
      sessions::SessionTabHelper::IdForTab(target_web_contents).id();
  LOG(INFO) << "[ExecutionService] StartRun target tab_id=" << target_tab_id
            << " url=" << target_web_contents->GetLastCommittedURL().spec()
            << " script_id=" << script_id
            << " actions=" << timeline.size()
            << " forced_native=" << (options.force_native_execution ? 1 : 0);

  if (runs_.size() >= max_concurrent_runs_) {
    LOG(ERROR) << "[ExecutionService] StartRun refused: max concurrency "
               << max_concurrent_runs_ << " reached";
    return std::string();
  }

  const std::string execution_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string context_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();

  ScriptContext* script_context = nullptr;
  if (persist && !script_id.empty()) {
    if (ScriptContextManager* mgr = ScriptContextManager::GetInstance()) {
      script_context =
          mgr->CreateContext(context_id, script_id, target_web_contents);
    }
  }

  auto executor = std::make_unique<::ScriptExecutor>(profile_);
  executor->SetCompletionCallback(
      base::BindOnce(&ScriptExecutionService::OnRunCompleted,
                     weak_ptr_factory_.GetWeakPtr(), execution_id));

  RunState state;
  state.executor = std::move(executor);
  state.context_id = context_id;
  state.web_contents = target_web_contents->GetWeakPtr();
  state.completion = std::move(completion);
  runs_.emplace(execution_id, std::move(state));

  // Note: ScriptExecutor already observes WebContents and will abort gracefully
  // if the tab is closed while the run is active.
  //
  // Completion is always posted asynchronously by ScriptExecutor, so it is
  // safe to start execution after inserting into |runs_|.
  base::Value::List timeline_copy = timeline.Clone();
  std::optional<base::Value::List> key_timeline_copy;
  if (key_timeline)
    key_timeline_copy = key_timeline->Clone();
  const base::FilePath variables_path = VariablesIniPath(profile_);
  const base::FilePath secrets_path = SecretsIniPath(profile_);
  const base::TimeTicks queued_at = base::TimeTicks::Now();

  bool has_cached_catalog = false;
  base::Time cached_variables_mtime;
  base::Time cached_secrets_mtime;
  {
    auto cache_it = TokenCatalogCacheByProfile().find(profile_);
    if (cache_it != TokenCatalogCacheByProfile().end() &&
        cache_it->second.valid) {
      has_cached_catalog = true;
      cached_variables_mtime = cache_it->second.variables_mtime;
      cached_secrets_mtime = cache_it->second.secrets_mtime;
    }
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&BuildExecutionTokenCatalogIfNeeded, variables_path,
                     secrets_path, has_cached_catalog, cached_variables_mtime,
                     cached_secrets_mtime),
      base::BindOnce(
          [](base::WeakPtr<ScriptExecutionService> weak_this,
             std::string run_id,
             std::string run_script_id,
             base::Value::List run_timeline,
             std::optional<base::Value::List> run_key_timeline,
             int run_start_step_index,
             std::string run_resume_url,
             RunOptions run_options,
             dashboard::ScriptContext* run_script_context,
             base::TimeTicks run_queued_at,
             TokenCatalogBuildResult build_result) {
            if (!weak_this)
              return;

            auto it = weak_this->runs_.find(run_id);
            if (it == weak_this->runs_.end() || !it->second.executor)
              return;
            content::WebContents* run_web_contents =
                it->second.web_contents.get();
            if (!run_web_contents) {
              auto completion = std::move(it->second.completion);
              weak_this->runs_.erase(it);
              if (completion) {
                std::move(completion).Run(false, -1,
                                          "Target WebContents was destroyed.");
              }
              return;
            }

            const ExecutionTokenCatalog* token_catalog = nullptr;
            const char* token_catalog_source = "cache";
            auto& cache_by_profile = TokenCatalogCacheByProfile();
            auto cache_it = cache_by_profile.find(weak_this->profile_);
            if (build_result.reuse_cached) {
              if (cache_it != cache_by_profile.end() && cache_it->second.valid) {
                token_catalog = &cache_it->second.catalog;
              }
            } else if (build_result.catalog.has_value()) {
              TokenCatalogCacheState& cache_state =
                  cache_by_profile[weak_this->profile_];
              cache_state.valid = true;
              cache_state.variables_mtime = build_result.variables_mtime;
              cache_state.secrets_mtime = build_result.secrets_mtime;
              cache_state.catalog = std::move(*build_result.catalog);
              token_catalog = &cache_state.catalog;
              token_catalog_source = "rebuilt";
            }
            if (!token_catalog && cache_it != cache_by_profile.end() &&
                cache_it->second.valid) {
              token_catalog = &cache_it->second.catalog;
            }
            if (!token_catalog) {
              static const base::NoDestructor<ExecutionTokenCatalog> empty_catalog;
              token_catalog = empty_catalog.get();
              token_catalog_source = "empty";
            }

            it->second.executor->SetForceCdpExecutionForRun(
                run_options.force_cdp_execution);
            it->second.executor->SetForceNativeExecutionForRun(
                run_options.force_native_execution);
            it->second.executor->SetAgentModeForRun(run_options.is_agent_mode);

            int replacement_count = 0;
            int locator_fanout_count = 0;
            base::Value::List expanded_timeline = ExpandTokensInTimelineOnePass(
                run_timeline, token_catalog->scalar_tokens,
                token_catalog->locator_value_lists, &replacement_count,
                &locator_fanout_count);

            std::optional<base::Value::List> expanded_key_timeline;
            if (run_key_timeline) {
              expanded_key_timeline = ExpandTokensInTimelineOnePass(
                  *run_key_timeline, token_catalog->scalar_tokens,
                  token_catalog->locator_value_lists, &replacement_count,
                  &locator_fanout_count);
            }
            // The executor only ever sees expanded timelines; give it the way
            // back so history / promoted params / training rows keep {Token}.
            it->second.executor->SetTokenReverseMapForRun(
                BuildTokenReverseMap(token_catalog->scalar_tokens));
            const base::Value::List* key_timeline_to_run =
                expanded_key_timeline ? &(*expanded_key_timeline) : nullptr;
            if (run_start_step_index >= 0) {
              it->second.executor->ExecuteScriptFromStep(
                  expanded_timeline, run_web_contents, run_script_context,
                  run_start_step_index, run_resume_url);
            } else {
              it->second.executor->ExecuteScript(
                  expanded_timeline, run_web_contents, run_script_context,
                  key_timeline_to_run);
            }

            const int64_t prep_delay_ms =
                (base::TimeTicks::Now() - run_queued_at).InMilliseconds();
            LOG(INFO) << "[ExecutionService] Started run id=" << run_id
                      << " script_id=" << run_script_id
                      << " actions=" << expanded_timeline.size()
                      << " forced_cdp="
                      << (run_options.force_cdp_execution ? 1 : 0)
                      << " forced_native="
                      << (run_options.force_native_execution ? 1 : 0)
                      << " token_expansions=" << replacement_count
                      << " locator_fanouts=" << locator_fanout_count
                      << " prep_delay_ms=" << prep_delay_ms
                      << " token_catalog_source=" << token_catalog_source
                      << " running=" << weak_this->runs_.size();
          },
          weak_ptr_factory_.GetWeakPtr(), execution_id, script_id,
          std::move(timeline_copy), std::move(key_timeline_copy),
          start_step_index, resume_url, options,
          script_context, queued_at));

  LOG(INFO) << "[ExecutionService] Queued run start id=" << execution_id
            << " script_id=" << script_id
            << " actions=" << timeline.size()
            << " forced_cdp=" << (options.force_cdp_execution ? 1 : 0)
            << " running=" << runs_.size();
  return execution_id;
}

void ScriptExecutionService::OnRunCompleted(const std::string& execution_id,
                                            bool success,
                                            int failed_step,
                                            std::string message) {
  auto it = runs_.find(execution_id);
  if (it == runs_.end()) {
    return;
  }

  CompletionCallback completion = std::move(it->second.completion);
  const std::string context_id = it->second.context_id;

  // Destroy the executor before tearing down ScriptContext / erasing RunState.
  // Otherwise ~ScriptExecutor can trip dangling-pointer checks against context
  // or WebContents state already cleared by RemoveContext.
  if (it->second.executor) {
    it->second.executor->CleanupAfterRun();
    it->second.executor.reset();
  }

  if (!context_id.empty()) {
    if (ScriptContextManager* mgr = ScriptContextManager::GetInstance()) {
      mgr->RemoveContext(context_id);
    }
  }

  LOG(INFO) << "[ExecutionService] Run finished id=" << execution_id
            << " success=" << success << " failed_step=" << failed_step
            << " message=" << message;
  runs_.erase(it);

  if (completion) {
    std::move(completion).Run(success, failed_step, std::move(message));
  }
}

}  // namespace dashboard
