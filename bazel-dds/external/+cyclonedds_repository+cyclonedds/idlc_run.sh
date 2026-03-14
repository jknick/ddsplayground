#!/usr/bin/env bash
# Wrapper that forwards all args to the CycloneDDS IDL compiler.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/_install/bin/idlc" "$@"
