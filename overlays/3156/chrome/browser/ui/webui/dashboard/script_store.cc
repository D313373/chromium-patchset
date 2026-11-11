#include "chrome/browser/ui/webui/dashboard/script_store.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"

namespace dashboard {
namespace {

bool IsSafePathSegment(const std::string& segment) {
  if (segment.empty() || segment.size() > 64) {
    return false;
  }
  if (segment == "." || segment == "..") {
    return false;
  }
  for (char c : segment) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

void LoadScriptsFromDirectory(const base::FilePath& dir,
                              const std::string& collection_prefix,
                              base::Value::Dict* out) {
  if (!out || !base::PathExists(dir)) {
    return;
  }
  base::FileEnumerator it(dir, /*recursive=*/false, base::FileEnumerator::FILES,
                          FILE_PATH_LITERAL("*.json"));
  for (base::FilePath path = it.Next(); !path.empty(); path = it.Next()) {
    std::string basename = path.BaseName().RemoveExtension().MaybeAsASCII();
    if (basename.empty() || basename == "global_settings") {
      continue;
    }
    if (!IsSafePathSegment(basename)) {
      LOG(WARNING) << "[ScriptStore] Skipping unsafe script name: "
                   << path.AsUTF8Unsafe();
      continue;
    }
    std::string json;
    if (!base::ReadFileToString(path, &json)) {
      PLOG(ERROR) << "[ScriptStore] ReadAll failed: " << path.AsUTF8Unsafe();
      continue;
    }
    auto val =
        base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!val || !val->is_dict()) {
      LOG(ERROR) << "[ScriptStore] ReadAll not dict: " << path.AsUTF8Unsafe();
      continue;
    }
    std::string id = collection_prefix.empty()
                         ? basename
                         : (collection_prefix + "/" + basename);
    base::Value::Dict dict = val->GetDict().Clone();
    if (!collection_prefix.empty()) {
      dict.Set("collection", collection_prefix);
    }
    dict.Set("id", id);
    out->Set(id, std::move(dict));
  }
}

}  // namespace

// Mirrors DashboardUIMessageHandler::ScriptsDir() default.
base::FilePath ScriptStore::ScriptsDir(Profile* profile) {
  if (!profile)
    return base::FilePath();
  if (const char* env = std::getenv("DASHBOARD_SCRIPTS_DIR"); env && *env) {
    return base::FilePath(env);
  }
  if (const char* home = std::getenv("SIMULACHROME_HOME"); home && *home) {
    return base::FilePath(home).AppendASCII("Scripts");
  }
  base::FilePath app_data_dir;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_data_dir) &&
      !app_data_dir.empty()) {
    return app_data_dir.AppendASCII("Simulachrome").AppendASCII("Scripts");
  }
  return profile->GetPath().AppendASCII("SimulachromeScripts");
}

bool ScriptStore::IsValidCollectionName(const std::string& name) {
  return IsSafePathSegment(name);
}

bool ScriptStore::IsValidScriptId(const std::string& id) {
  if (id.empty() || id.find("..") != std::string::npos ||
      base::StartsWith(id, "/") || base::EndsWith(id, "/")) {
    return false;
  }
  const size_t slash = id.find('/');
  if (slash == std::string::npos) {
    return IsSafePathSegment(id);
  }
  if (id.find('/', slash + 1) != std::string::npos) {
    return false;
  }
  return IsSafePathSegment(id.substr(0, slash)) &&
         IsSafePathSegment(id.substr(slash + 1));
}

bool ScriptStore::IsCollectionRef(const std::string& value) {
  return base::StartsWith(value, kCollectionRefPrefix,
                          base::CompareCase::INSENSITIVE_ASCII);
}

std::string ScriptStore::ParseCollectionRef(const std::string& value) {
  if (!IsCollectionRef(value)) {
    return std::string();
  }
  std::string name = value.substr(sizeof(kCollectionRefPrefix) - 1);
  base::TrimWhitespaceASCII(name, base::TRIM_ALL, &name);
  return name;
}

std::string ScriptStore::FormatCollectionRef(const std::string& collection_name) {
  if (collection_name.empty()) {
    return std::string();
  }
  return std::string(kCollectionRefPrefix) + collection_name;
}

std::string ScriptStore::CollectionOfScriptId(const std::string& id) {
  const size_t slash = id.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= id.size()) {
    return std::string();
  }
  return id.substr(0, slash);
}

base::FilePath ScriptStore::PathForScriptId(Profile* profile,
                                            const std::string& id) {
  if (!IsValidScriptId(id)) {
    return base::FilePath();
  }
  const base::FilePath dir = ScriptsDir(profile);
  const size_t slash = id.find('/');
  if (slash == std::string::npos) {
    return dir.AppendASCII(id + ".json");
  }
  return dir.AppendASCII(id.substr(0, slash))
      .AppendASCII(id.substr(slash + 1) + ".json");
}

bool ScriptStore::Read(Profile* profile,
                       const std::string& id,
                       base::Value::Dict* out) {
  if (!out || !IsValidScriptId(id))
    return false;
  const base::FilePath path = PathForScriptId(profile, id);
  if (path.empty())
    return false;
  std::string json;
  if (!base::PathExists(path)) {
    VLOG(1) << "[ScriptStore] Read missing: " << path.AsUTF8Unsafe();
    return false;
  }
  if (!base::ReadFileToString(path, &json)) {
    PLOG(ERROR) << "[ScriptStore] Read failed: " << path.AsUTF8Unsafe();
    return false;
  }
  auto val = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!val || !val->is_dict()) {
    LOG(ERROR) << "[ScriptStore] JSON not a dict: " << path.AsUTF8Unsafe();
    return false;
  }
  *out = val->GetDict().Clone();
  const std::string collection = CollectionOfScriptId(id);
  if (!collection.empty()) {
    out->Set("collection", collection);
  }
  out->Set("id", id);
  return true;
}

bool ScriptStore::Write(Profile* profile,
                        const std::string& id,
                        const base::Value::Dict& in) {
  if (!IsValidScriptId(id)) {
    LOG(ERROR) << "[ScriptStore] Write rejected invalid id=" << id;
    return false;
  }
  const base::FilePath path = PathForScriptId(profile, id);
  if (path.empty()) {
    return false;
  }
  if (!base::CreateDirectory(path.DirName())) {
    PLOG(ERROR) << "[ScriptStore] CreateDirectory failed: "
                << path.DirName().AsUTF8Unsafe();
    return false;
  }
  std::string json;
  base::JSONWriter::Write(in, &json);
  int rv = base::WriteFile(path, base::as_byte_span(json));
  if (rv <= 0) {
    PLOG(ERROR) << "[ScriptStore] Write failed: " << path.AsUTF8Unsafe()
                << ", rv=" << rv;
    return false;
  }
  VLOG(1) << "[ScriptStore] Wrote: " << path.AsUTF8Unsafe() << ", bytes=" << rv;
  return true;
}

bool ScriptStore::Delete(Profile* profile, const std::string& id) {
  if (!IsValidScriptId(id)) {
    return false;
  }
  const base::FilePath path = PathForScriptId(profile, id);
  if (path.empty() || !base::PathExists(path)) {
    return true;
  }
  if (!base::DeleteFile(path)) {
    PLOG(ERROR) << "[ScriptStore] Delete failed: " << path.AsUTF8Unsafe();
    return false;
  }
  VLOG(1) << "[ScriptStore] Deleted: " << path.AsUTF8Unsafe();
  return true;
}

bool ScriptStore::EnsureCollection(Profile* profile,
                                   const std::string& collection_name) {
  if (!IsValidCollectionName(collection_name)) {
    return false;
  }
  const base::FilePath dir =
      ScriptsDir(profile).AppendASCII(collection_name);
  if (!base::CreateDirectory(dir)) {
    PLOG(ERROR) << "[ScriptStore] EnsureCollection failed: "
                << dir.AsUTF8Unsafe();
    return false;
  }
  return true;
}

std::vector<std::string> ScriptStore::ListCollections(Profile* profile) {
  std::vector<std::string> out;
  const base::FilePath dir = ScriptsDir(profile);
  if (!base::PathExists(dir)) {
    return out;
  }
  base::FileEnumerator it(dir, /*recursive=*/false,
                          base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = it.Next(); !path.empty(); path = it.Next()) {
    std::string name = path.BaseName().MaybeAsASCII();
    if (IsValidCollectionName(name)) {
      out.push_back(name);
    }
  }
  return out;
}

base::Value::Dict ScriptStore::LoadAll(Profile* profile) {
  base::Value::Dict out;
  const base::FilePath dir = ScriptsDir(profile);
  if (!base::PathExists(dir))
    return out;

  LoadScriptsFromDirectory(dir, /*collection_prefix=*/"", &out);

  for (const std::string& collection : ListCollections(profile)) {
    LoadScriptsFromDirectory(dir.AppendASCII(collection), collection, &out);
  }
  return out;
}

}  // namespace dashboard
