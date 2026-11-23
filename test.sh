#!/usr/bin/env bash
# Every hardware-free test: firmware host (CMake + ctest), server (pytest), Android :core (Gradle).
# Needs cmake, a C compiler, python3 and a JDK >= 17. Pytest goes into server/.venv on first run.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"

echo "== firmware: hb_core host tests"
gen=""
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
cmake -S "$root/fw/test/host" -B "$root/fw/test/host/build" $gen
cmake --build "$root/fw/test/host/build"
(cd "$root/fw/test/host/build" && ctest --output-on-failure)

echo "== server: pytest"
venv="$root/server/.venv"
py=python3
command -v python3 >/dev/null 2>&1 || py=python
[ -d "$venv" ] || "$py" -m venv "$venv"
vpy="$venv/bin/python"
[ -x "$vpy" ] || vpy="$venv/Scripts/python.exe"
"$vpy" -m pip install -q --disable-pip-version-check "pytest>=8"
(cd "$root/server" && "$vpy" -m pytest -q)

echo "== android: :core tests"
if [ -n "${JAVA_HOME:-}" ] && [ ! -d "$JAVA_HOME" ]; then
  echo "JAVA_HOME=$JAVA_HOME does not exist; using java from PATH" >&2
  unset JAVA_HOME
fi
(cd "$root/android" && ./gradlew test --no-daemon --console=plain)

echo "== all suites passed"
