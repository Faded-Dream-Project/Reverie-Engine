#!/bin/sh
# ============================================================================
# Reverie Engine — scripts/push.sh
#
# Pushes to the GitHub remote using a token supplied ONLY through the
# environment (GITHUB_TOKEN). The token is never written to any file, never
# appears in argv of a persisted command line beyond this transient process,
# and never lands in the repository or its history.
#
# Usage:
#   GITHUB_TOKEN=github_pat_... ./scripts/push.sh [remote-branch] [commit...]
#
# Prefer a real credential helper for day-to-day work instead:
#   git config credential.helper store      # ~/.git-credentials, chmod 600
#   git config credential.helper 'cache --timeout=3600'
# or SSH remotes. This script exists for CI-ish one-shot pushes from
# environments where those are unavailable.
#
# SECURITY: if a token has ever been pasted into a chat, a log, or a file,
# treat it as compromised and rotate it at:
#   https://github.com/settings/tokens
# ============================================================================
set -e

BRANCH="${1:-main}"
# NB: 'shift' on empty $@ is a FATAL special-builtin error in dash (POSIX
# mode) even under '|| true' — guard it instead.
if [ $# -gt 0 ]; then
    shift
fi

if [ -z "$GITHUB_TOKEN" ]; then
    echo "push.sh: GITHUB_TOKEN is not set." >&2
    echo "  export GITHUB_TOKEN=<your personal access token> and retry." >&2
    echo "  (never paste tokens into chats, commits, or logs)" >&2
    exit 1
fi

REMOTE_URL="https://${GITHUB_TOKEN}@github.com/FemBoyGamerTechGuy/Reverie-Engine.git"

# The token-bearing URL exists only inside this process; git stores it in
# neither the config nor the remote list. Stderr is filtered so an aborted
# push cannot echo the URL back.
git push "$REMOTE_URL" "$BRANCH" 2>&1 | sed "s|${GITHUB_TOKEN}|<token>|g"
status=${PIPESTATUS:-0}

exit $status
