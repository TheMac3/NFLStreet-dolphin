# NFLStreetRollback launcher (vendored)

Copied from the NFLStreet repo (`tools/`, commit ef5ad0a) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` (with `nsr_controllers.py`, `nsr_gamestats.py`, `nsr_textures.py`, the content-update
modules `nsr_content.py`/`rsasig.py`/`eadb.py`/`discpatch.py`,
`draftmodes.py` for Pickup policies, the experimental guarded retail recipe
`ownimage*.py` (metadata only, never game payloads), and the bundled
`nsr_launcher_ui.html` web UI, shown through pywebview/WebView2) into
"NFLStreetRollback.exe" and `nsr_patcher.py` into NFLStreetRollback-Patcher.exe with
PyInstaller and ships both in the friends zip.
