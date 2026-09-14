#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptDir, "../../..");
const read = (relative) =>
  fs.readFileSync(path.join(root, relative), "utf8");
const requireText = (relative, needles) => {
  const text = read(relative);
  for (const needle of needles) {
    if (!text.includes(needle)) {
      throw new Error(`${relative}: missing contract marker ${needle}`);
    }
  }
};

const accessRoot = "apps/browser/overlay/chrome/browser/aegis/access";
requireText(`${accessRoot}/access_rule_store.cc`, [
  "meta_table_.Init",
  "sql::Transaction",
  "access_store_identity",
  "storage_partition_token",
  "target_mode INTEGER NOT NULL",
  "publish_started INTEGER NOT NULL",
  "ValidateSiteProxyRuleGroup",
  "DELETE FROM access_rules WHERE storage_partition_token=? AND",
  "UPDATE access_pending_operations SET state=2",
  "MarkPublishStarted",
  "RebindOperationForRecovery",
]);
requireText(`${accessRoot}/site_proxy_toggle_coordinator.cc`, [
  "ChannelNamespace::kBeta",
  "ChannelNamespace::kRelease",
  "PrepareRoute",
  "PublishPolicy",
  "CommitAndRelease",
  "EnterFailClosed",
  "EnterStoreFailClosed",
  "EventMatches",
  "TryPublishNextReady",
]);
requireText(`${accessRoot}/site_proxy_toggle_coordinator.h`, [
  "BrowserConfirmedSite",
  "RouteReadyEvent",
  "PolicyAckEvent",
  "ProxyRuntimeState",
]);

const series = read("apps/browser/patches/series")
  .split(/\r?\n/)
  .filter(Boolean);
const patchName =
  "0115-feat-aegis-persist-site-toggle-transactions.patch";
if (series.at(-1) !== patchName) {
  throw new Error(`patch 0115 must be the final series entry`);
}
requireText(`apps/browser/patches/${patchName}`, [
  "chrome/browser/aegis/access/access_rule_store.cc",
  "chrome/browser/aegis/access/site_proxy_toggle_coordinator.cc",
  "chrome/browser/aegis/access/access_rule_store_unittest.cc",
  "chrome/browser/aegis/access/site_proxy_toggle_coordinator_unittest.cc",
  "chrome/browser/aegis/access/BUILD.gn",
  "chrome/browser/aegis/BUILD.gn",
  "tools/metrics/histograms/metadata/sql/histograms.xml",
  'variant name="AegisAccess"',
]);

console.log(
  "access-rule-store contract: PASS (store, coordinator, tests, BUILD, patch 0115)",
);
