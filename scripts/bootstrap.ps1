param(
  [ValidateSet("debug", "release", "msvc-debug", "msvc-release")]
  [string]$Preset = "debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

cmake --preset $Preset
cmake --build --preset $Preset
ctest --preset $Preset
