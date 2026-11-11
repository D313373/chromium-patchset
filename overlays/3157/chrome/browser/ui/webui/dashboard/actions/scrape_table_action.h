#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCRAPE_TABLE_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCRAPE_TABLE_ACTION_H_

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include <memory>
#include <utility>
#include <vector>

class ScrapeTableAction : public dashboard::actions::Action {
 public:
  explicit ScrapeTableAction(Profile* profile);
  ~ScrapeTableAction() override;

  // Action implementations
  void Execute(content::WebContents* web_contents, Callback callback) override;
 bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnNativePageMapCaptured(content::WebContents* web_contents,
                               Callback callback,
                               dashboard::UnifiedPageMapResult result);
  void StartResolverPageMapFallback(content::WebContents* web_contents,
                                    Callback callback,
                                    const std::string& reason);
  void StartNativeLocatorMatchFallback(content::WebContents* web_contents,
                                       Callback callback,
                                       const std::string& reason);
  void OnNativeLocatorMatchesReady(Callback callback,
                                   const std::string& reason,
                                   base::Value::Dict page_map);
  void OnResolverPageMapFallbackReady(content::WebContents* web_contents,
                                      Callback callback,
                                      const std::string& reason,
                                      base::Value::Dict resolver_page_map);
  void FinalizeRowsPayload(Callback callback,
                           base::Value::List rows_json,
                           bool used_native_sweep);
  void OnCsvWriteFinished(Callback callback,
                          const std::string& payload_json,
                          bool make_unique_if_needed,
                          const base::FilePath& requested_csv_path,
                          std::pair<bool, base::FilePath> write_result);

  std::string variable_name_;
  std::vector<dashboard::Locator> row_locators_;
  std::vector<dashboard::ScrapeTableField> fields_;
  bool row_index_mode_ = false;
  std::vector<int> row_indices_zero_based_;
  bool save_csv_ = true;
  bool append_csv_ = false;
  bool csv_file_name_explicit_ = false;
  std::string csv_file_name_;

  bool resolver_page_map_fallback_attempted_ = false;
  std::unique_ptr<dashboard::PageMapExtractor> resolver_page_map_extractor_;

  base::WeakPtrFactory<ScrapeTableAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCRAPE_TABLE_ACTION_H_
