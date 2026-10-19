# NFLStreetRollback launcher (vendored)

Copied from the NFLStreet repo (`tools/`, commit c82d4fa) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` (with `nsr_controllers.py`, `nsr_gamestats.py` and the bundled
`nsr_launcher_ui.html` web UI, shown through pywebview/WebView2) into
"NFLStreetRollback.exe" and `nsr_patcher.py` into NFLStreetRollback-Patcher.exe with
PyInstaller and ships both in the friends zip.


Bootsync launcher delta synchronized from NFLStreet commit `76499fc`:
named preboot failures are shown in the launcher and the compatibility image
fingerprint remains present when telemetry is disabled. The rest of this
vendored baseline remains at the source commit above; unrelated content-update
integration is a separate synchronization step.
