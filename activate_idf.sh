# Activates the self-contained ESP-IDF environment for this workspace.
# Usage (from the workspace root, in bash or zsh):
#   . ./activate_idf.sh
#
# Everything (toolchains, Python venv, downloaded tools) lives under
# .espressif/ inside this workspace - nothing is installed system-wide
# or into the user's home directory.

if [ -n "${BASH_SOURCE-}" ] && [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "This script must be sourced, not executed:"
    echo ". ${BASH_SOURCE[0]}"
    exit 1
fi

if [ -n "${BASH_SOURCE-}" ]; then
    workspace_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
elif [ -n "${ZSH_VERSION-}" ]; then
    workspace_root=$(cd "$(dirname "${(%):-%x}")" && pwd)
else
    workspace_root=$(pwd)
fi

export IDF_TOOLS_PATH="$workspace_root/.espressif"
. "$workspace_root/esp-idf/export.sh"
unset workspace_root
