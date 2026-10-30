# NFLStreetRollback launcher (vendored)

Copyright (C) 2026 Mason Blanchard. SPDX-License-Identifier: GPL-2.0-or-later.
These files are a separate program from Dolphin - they run it as a subprocess
and link none of its code - and carry the same license so that no file in this
tree is left without one. The NFLStreetRollback name and marks are not
licensed under it; see the NFLStreet repository's NOTICE.

Copied from the NFLStreet repo (`tools/`, commit 82ba795) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` (with `nsr_controllers.py`, `nsr_gamestats.py`, `nsr_textures.py`, the content-update
modules `nsr_content.py`/`nsr_roster.py`/`rsasig.py`/`eadb.py`/`discpatch.py`/`ratingsplit.py`,
`nsr_playbooks.py`/`playbooks.py`/`ealzh.py` for signed personal books,
`draftmodes.py` for Pickup policies, the guarded owned-disc recipe
`ownimage*.py` (metadata only, never game payloads), and the bundled
`nsr_launcher_ui.html` web UI, shown through pywebview/WebView2) into
"NFLStreetRollback.exe" and `nsr_patcher.py` into NFLStreetRollback-Patcher.exe with
PyInstaller and ships both in the friends zip.
