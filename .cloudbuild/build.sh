#!/bin/sh
# Company pipeline entry point; works independently of the caller's current directory.
set -eu
AME_REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
exec python3 "$AME_REPO_ROOT/.cloudbuild/build.py" "$@"
