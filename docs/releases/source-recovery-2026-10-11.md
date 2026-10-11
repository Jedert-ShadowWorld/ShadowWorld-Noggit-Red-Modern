# Source recovery, 2026-10-11

Canonical source: `I:/WOW/Server/Codex/Noggit`.
Normal runtime: `C:/NoggitBuild/Shadow-Noggit/bin/Release/noggit.exe`.
Build directory: `C:/NoggitBuild/Shadow-Noggit-Recovered`.
Build command: `tools/build-recovered.cmd` (six parallel jobs).

Recovered from Jedert-ShadowWorld/ShadowWorld-Noggit-Red-Modern,
branch shadowworld-modern-retail-preview, base 0c41c2fa.
Local changes restore MTXP/MHID height rendering, texture metadata preservation,
bulk DB2 creation and texture-only split ADT placement preservation, including
existing M2 flags, derived WMO bounds and normalized rotation comparison.
Actual object edits remain subject to supported OBJ1 layout validation.

Listfile refresh now checks GitHub release metadata before downloading.
ETag, asset identity and the release digest avoid unchanged CSV downloads.
Downloads are validated and committed atomically. Network failures retain a
usable existing CSV. Unit tests and two live checks passed without redownloading
the project's unchanged listfile.

Regression checks passed for texture metadata, terrain shader rendering,
bulk DB2 creation, split save rollback, new map creation, map DB2 editing,
editor DB2 tables and OBJ1 preservation. Real editor save/reopen verification
is still pending. Recovery does not prove identity with the entire deleted tree.

Source backup: `C:/NoggitBuild/Packages/ShadowWorld-Noggit-RecoveredSource-20261011.zip`.
The backup includes current local source changes; no GitHub push was performed.
