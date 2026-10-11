# NSR Netplay launcher (vendored)

Copied from the NFLStreet repo (`tools/`, commit d28e949) by
`scripts/sync-launcher-to-fork.sh`. Don't edit here; edit in NFLStreet and
re-sync. The Windows CI workflow (`.github/workflows/nsr-windows.yml`)
builds `nsr_launcher.py` into "NSR Netplay.exe" and `nsr_patcher.py` into
NSR-Patcher.exe with PyInstaller and ships both in the friends zip.
