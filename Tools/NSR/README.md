# NFLStreetRollback launcher (vendored)

Copied from the NFLStreet repo (`tools/`, commit c29f43c) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` (with `nsr_controllers.py`, `nsr_gamestats.py`, the content-update
modules `nsr_content.py`/`rsasig.py`/`eadb.py`/`discpatch.py` and the bundled
`nsr_launcher_ui.html` web UI, shown through pywebview/WebView2) into
"NFLStreetRollback.exe" and `nsr_patcher.py` into NFLStreetRollback-Patcher.exe with
PyInstaller and ships both in the friends zip.
