#!/usr/bin/env bash
# Build and stage only. Never installs/restarts services or accesses the gateway DB.
set -euo pipefail
umask 077
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
baseline=a4c17adfa04abc63637ad17de7f85256a825a2cb
[[ $# -le 1 ]] || { echo 'Usage: bash tools/build-alarm-users.sh [reviewed-feature-commit]'; exit 1; }
feature=$(git rev-parse --verify --end-of-options "${1:-HEAD}^{commit}")
for program in git cmake pkg-config g++ python3; do
    command -v "$program" >/dev/null || { echo "Missing build dependency: $program. See BUILDING.md."; exit 1; }
done
pkg-config --exists Qt5Core Qt5Widgets Qt5Network Qt5WebSockets Qt5SerialPort Qt5Qml sqlite3 openssl || {
    echo 'Missing Qt5/SQLite/OpenSSL development packages. See BUILDING.md.'; exit 1;
}
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    echo 'Tracked working files are modified. Commit or set them aside before building a reproducible snapshot.'
    exit 1
fi
git cat-file -e "$baseline^{commit}"
mkdir -p .local-builds
build_root=$(mktemp -d "$repo_root/.local-builds/alarm-users.XXXXXXXX")
echo "Build output: $build_root"
{
    echo "baseline=$baseline"
    echo "feature=$feature"
    echo "qt=$(pkg-config --modversion Qt5Core)"
    dpkg-query -W -f='installed_deconz=${Version}\n' deconz 2>/dev/null || true
} > "$build_root/versions.txt"
# Resolve the library only once so both plugin builds use identical library sources.
library_revision=$(git ls-remote https://github.com/dresden-elektronik/deconz-lib.git refs/heads/main | cut -f1)
[[ "$library_revision" =~ ^[0-9a-f]{40}$ ]] || { echo 'Could not resolve deconz-lib revision.'; exit 1; }
echo "deconz_lib=$library_revision" >> "$build_root/versions.txt"
for variant in baseline feature; do
    revision=$baseline
    [[ "$variant" == feature ]] && revision=$feature
    source_dir="$build_root/$variant-source"
    git worktree add --detach "$source_dir" "$revision"
    # Only the dependency revision is pinned in each temporary source worktree.
    python3 - "$source_dir/CMakeLists.txt" "$library_revision" <<'PYCODE'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = "GIT_TAG        main"
if text.count(old) != 1:
    raise SystemExit("Unexpected dependency declaration; build stopped")
path.write_text(text.replace(old, "GIT_TAG        " + sys.argv[2]))
PYCODE
    cmake -S "$source_dir" -B "$build_root/$variant-build" \
        -DQT_VERSION_MAJOR=5 -DCMAKE_INSTALL_PREFIX=/usr \
        2>&1 | tee "$build_root/$variant-configure.log"
    cmake --build "$build_root/$variant-build" --parallel 2 \
        2>&1 | tee "$build_root/$variant-build.log"
    cmake --install "$build_root/$variant-build" --prefix "$build_root/$variant-stage" \
        2>&1 | tee "$build_root/$variant-stage.log"
done
g++ -std=c++14 -Wall -Wextra -Werror -I"$build_root/feature-source" \
    "$build_root/feature-source/alarm_user_store.cpp" \
    "$build_root/feature-source/tests/alarm_users_test.cpp" \
    -lsqlite3 -lcrypto -pthread -o "$build_root/alarm_users_test"
(cd "$build_root" && ./alarm_users_test) | tee "$build_root/tests.log"
sha256sum "$build_root/"{baseline,feature}-stage/share/deCONZ/plugins/libde_rest_plugin.so > "$build_root/plugin-hashes.txt"
echo 'Both plugin builds and user-store tests passed. Nothing installed or restarted.'
echo "Keep the staged binaries and build records at: $build_root"
echo 'Installation still requires matching the installed executable/Qt ABI and a private recovery snapshot.'
