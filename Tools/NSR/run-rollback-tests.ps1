# Compile and run the socket-free NSR rollback regression test from a Visual
# Studio Developer PowerShell. The output stays in the caller's temporary
# directory, so this also works as a minimal Windows CI step without building
# Dolphin or changing the checked-in Visual Studio solution.

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Output = Join-Path ([System.IO.Path]::GetTempPath()) "nsr-rollback-tests.exe"

& cl.exe /nologo /std:c++20 /EHsc /W4 /WX /UNDEBUG `
  "/I$Root\Source\Core" `
  "$Root\Tools\NSR\NSRRollbackNetplayTest.cpp" `
  "$Root\Source\Core\Core\NSR\NSRRollbackNetplay.cpp" `
  "/Fe:$Output"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Output
exit $LASTEXITCODE
