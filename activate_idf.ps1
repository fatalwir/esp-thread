# Activates the self-contained ESP-IDF environment for this workspace.
# Usage (from the workspace root, in PowerShell):
#   . .\activate_idf.ps1
#
# Everything (toolchains, Python venv, downloaded tools) lives under
# .espressif\ inside this workspace - nothing is installed system-wide
# or into the user's home profile.

$WorkspaceRoot = $PSScriptRoot
$env:IDF_TOOLS_PATH = Join-Path $WorkspaceRoot ".espressif"

Push-Location (Join-Path $WorkspaceRoot "esp-idf")
try {
    . .\export.ps1
}
finally {
    Pop-Location
}
