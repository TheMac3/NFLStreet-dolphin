# NSR Netplay launcher (vendored)

Copied from the NFLStreet repo (`tools/`, commit 5d495d5) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` (with `nsr_controllers.py` and the bundled
`nsr_launcher_ui.html` web UI, shown through pywebview/WebView2) into
"NSR Netplay.exe" and `nsr_patcher.py` into NSR-Patcher.exe with
PyInstaller and ships both in the friends zip.
