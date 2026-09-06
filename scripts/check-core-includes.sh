#!/usr/bin/env bash
#
# Enforces the one rule that keeps this architecture alive:
#
#   NOTHING UNDER core/ MAY INCLUDE Qt, SQLite, JNI OR PLATFORM HEADERS.
#
# That rule is what makes the core unit-testable with no device, what makes the
# loopback transport possible, and what keeps the iOS port to "write one radio
# class" instead of "port the protocol". It erodes silently — one convenient
# QString at a time — so it is checked mechanically on every push.
#
# See docs/ARCHITECTURE.md §04.
#
# Usage:  scripts/check-core-includes.sh
# Exit:   0 clean, 1 violation found

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$ROOT/core"

# Header prefixes the core must never depend on.
FORBIDDEN='^[[:space:]]*#include[[:space:]]*[<"](Q[A-Z]|qt|sqlite3|jni\.h|android/|CoreBluetooth|Foundation/)'

echo "Checking core/ for forbidden includes..."

if ! violations="$(grep -rnE "$FORBIDDEN" "$CORE" --include='*.h' --include='*.cpp' 2>/dev/null)"; then
    echo "OK — core/ is free of Qt, SQLite, JNI and platform headers."
    exit 0
fi

echo
echo "ARCHITECTURE VIOLATION — core/ must stay platform-free:"
echo
echo "$violations"
echo
echo "Fix: move the platform-dependent code into adapters/ and depend on an"
echo "interface in core/ports/ instead. See docs/ARCHITECTURE.md section 04."
exit 1
