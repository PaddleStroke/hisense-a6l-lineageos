A6L LineageOS Updater feed placeholders (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md, format and hosting:
docs/release-prep-20260927.md section 2).
- a6l.json = `[]`: the initial content of `updater/a6l.json` on `main` of the public repo (PaddleStroke/hisense-a6l-lineageos) that
  rom/release/release.mk points release builds at (lineage.updater.uri=.../main/updater/{device}.json). An empty v2 list =
  "no update available" (the Updater shows no error). Each release appends itself with
  `python3 tools/release/make-updater-json.py <ota.zip> --url <https asset url> --merge updater/a6l.json`.
- Non-release builds (test keys, userdebug) point at .../updater/unpublished/{device}.json (rom/android/android.mk), which must
  NEVER be created: those images must not be offered a release-key OTA.
- Pierre: public or private hosting, and whether the Lineage recovery replaces the V74 diagnostic recovery (OTA install path).
