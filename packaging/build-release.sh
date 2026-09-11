#!/usr/bin/env bash
set -euo pipefail

# Environment overrides:
#   VERSION             Release version (default: 1.0.4)
#   WINDOWS_EXE         Built native executable
#   DIST_DIR            Output directory
#   PLATFORMS           Space-separated list: "windows", "source", or both
#   MAKE                make program (default: make, or mingw32-make when present)
#   MAKE_ARGS           Extra make arguments, for example CROSS= on a native Windows toolchain

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

VERSION="${VERSION:-1.0.4}"
PLATFORMS="${PLATFORMS:-windows source}"
WINDOWS_EXE="${WINDOWS_EXE:-$ROOT_DIR/build/windows/OutlastRequeue.exe}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/release}"
if [[ -z "${MAKE:-}" ]]; then
    if command -v mingw32-make >/dev/null 2>&1 && ! command -v make >/dev/null 2>&1; then
        MAKE=mingw32-make
    else
        MAKE=make
    fi
fi
MAKE_ARGS="${MAKE_ARGS:-}"

EXPECTED_BUILD_ID="25112110"
EXPECTED_VERIFY_TIMEOUT_MS="25000"
EXPECTED_AUTHOR="whispersgone"
CREATED_ARCHIVES=()

die() {
    echo "release build: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

wants_platform() {
    local requested
    for requested in $PLATFORMS; do
        [[ "$requested" == "$1" ]] && return 0
    done
    return 1
}

assert_clean_payload() {
    local directory="$1"
    local findings

    # Refuse common personal-path and credential forms.
    findings="$(
        grep -a -n -r -E \
            -e '/home/[A-Za-z0-9._-]+/' \
            -e '[A-Za-z]:\\Users\\[^\\[:space:]]+' \
            -e '0x[[:xdigit:]]{64}' \
            -e '[Cc]redential[[:space:]]*[:=][[:space:]]*[^[:space:]]+' \
            -e '[Pp]assword[[:space:]]*[:=][[:space:]]*[^[:space:]]+' \
            -e '[Ss]ecret[[:space:]]*[:=][[:space:]]*[^[:space:]]+' \
            "$directory" || true
    )"
    if [[ -n "$findings" ]]; then
        printf '%s\n' "$findings" >&2
        die "release payload contains a personal path or credential-shaped value"
    fi
}

write_tree_checksums() {
    local directory="$1"
    (
        cd -- "$directory"
        find . -type f ! -name SHA256SUMS -print0 \
            | LC_ALL=C sort -z \
            | xargs -0 sha256sum
    ) > "$directory/SHA256SUMS"
}

make_zip() {
    local stage_root="$1"
    local package_name="$2"
    local destination="$DIST_DIR/$package_name.zip"

    rm -f -- "$destination"
    if command -v zip >/dev/null 2>&1; then
        (
            cd -- "$stage_root"
            zip -X -q -r "$destination" "$package_name"
        )
    else
        (
            cd -- "$stage_root"
            "$PYTHON" - "$destination" "$package_name" <<'EOF'
import os, sys, zipfile
destination, name = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
    for root, directories, files in os.walk(name):
        directories.sort()
        for file in sorted(files):
            path = os.path.join(root, file)
            archive.write(path, path.replace(os.sep, "/"))
EOF
        )
    fi
    CREATED_ARCHIVES+=("$destination")
    echo "created $destination"
}

copy_source_file() {
    local relative_path="$1"
    local mode="$2"
    local destination_root="$3"
    local source="$ROOT_DIR/$relative_path"
    local destination="$destination_root/$relative_path"

    [[ -f "$source" && ! -L "$source" ]] \
        || die "source allowlist entry is missing, not regular, or a symlink: $relative_path"
    mkdir -p -- "$(dirname -- "$destination")"
    install -m "$mode" "$source" "$destination"
}

assert_source_payload() {
    local directory="$1"
    local finding

    finding="$(find "$directory" -type l -print -quit)"
    [[ -z "$finding" ]] || die "source payload contains a symlink: $finding"
    finding="$(find "$directory" -type f \
        \( -iname '*.pak' -o -iname '*.uasset' -o -iname '*.uexp' \) \
        -print -quit)"
    [[ -z "$finding" ]] \
        || die "source payload contains a game asset: $finding"
}

verify_release_constants() {
    grep -qE '#define RQ_DEFAULT_VERIFY_TIMEOUT_MS UINT64_C\('"$EXPECTED_VERIFY_TIMEOUT_MS"'\)' \
        "$ROOT_DIR/src/common/requeue_engine.h" \
        || die "C confirmation timeout does not match release metadata"
    grep -qE '"confirmation_timeout_ms":[[:space:]]*'"$EXPECTED_VERIFY_TIMEOUT_MS" \
        "$SCRIPT_DIR/compatibility.json" \
        || die "compatibility.json confirmation timeout is missing or stale"
    grep -qE 'SUPPORTED_BUILD_ID L"'"$EXPECTED_BUILD_ID"'"' \
        "$ROOT_DIR/src/windows/outlast_requeue_windows.c" \
        || die "Windows last-verified build ID is stale"
    grep -qE 'APP_VERSION L"'"$VERSION"'"' \
        "$ROOT_DIR/src/windows/outlast_requeue_windows.c" \
        || die "Windows APP_VERSION does not match $VERSION"
    grep -qE '"author":[[:space:]]*"'"$EXPECTED_AUTHOR"'"' \
        "$SCRIPT_DIR/compatibility.json" \
        || die "release author attribution is missing or stale"
}

build_and_verify_windows() {
    local default_executable="$ROOT_DIR/build/windows/OutlastRequeue.exe"

    if [[ "$WINDOWS_EXE" == "$default_executable" ]]; then
        # shellcheck disable=SC2086
        "$MAKE" -C "$ROOT_DIR/src/windows" clean all $MAKE_ARGS
    fi
    [[ -f "$WINDOWS_EXE" ]] || die "Windows executable not found: $WINDOWS_EXE"
    "$ROOT_DIR/tests/audit_windows_binary.sh" "$WINDOWS_EXE"
    file "$WINDOWS_EXE" | grep -qE 'PE32\+ executable.*GUI.*x86-64' \
        || die "Windows payload is not a PE64 GUI executable"
    strings -el "$WINDOWS_EXE" | grep -Fxq "$VERSION" \
        || die "Windows executable does not embed release version $VERSION"
    strings -el "$WINDOWS_EXE" | grep -Fxq "$EXPECTED_BUILD_ID" \
        || die "Windows executable does not embed build ID $EXPECTED_BUILD_ID"
    strings -el "$WINDOWS_EXE" | grep -q "$EXPECTED_AUTHOR" \
        || die "Windows executable does not embed creator attribution"
    "$WINDOWS_EXE" --self-test | grep -q SELF_TEST_PASS \
        || die "Windows executable self-test failed"
}

stage_common_docs() {
    local destination="$1"
    install -m 0644 "$SCRIPT_DIR/README.md" "$destination/PROJECT.md"
    install -m 0644 "$SCRIPT_DIR/compatibility.json" "$destination/compatibility.json"
    install -m 0644 "$SCRIPT_DIR/CHANGELOG.md" "$destination/CHANGELOG.md"
    install -m 0644 "$SCRIPT_DIR/LICENSE-CODE" "$destination/LICENSE-CODE"
    install -m 0644 "$SCRIPT_DIR/THIRD_PARTY_NOTICES.md" "$destination/THIRD_PARTY_NOTICES.md"
}

build_windows_archive() {
    [[ -f "$WINDOWS_EXE" ]] || die "Windows executable not found: $WINDOWS_EXE"

    local package_name="Outlast-Requeue-v$VERSION-Windows-x64-build$EXPECTED_BUILD_ID"
    local destination="$STAGE_ROOT/$package_name"
    mkdir -p -- "$destination"

    install -m 0755 "$WINDOWS_EXE" "$destination/Outlast Requeue.exe"
    install -m 0644 "$SCRIPT_DIR/windows/README.md" "$destination/README.md"
    stage_common_docs "$destination"

    assert_clean_payload "$destination"
    write_tree_checksums "$destination"
    make_zip "$STAGE_ROOT" "$package_name"
}

build_source_archive() {
    local package_name="Outlast-Requeue-v$VERSION-Source"
    local destination="$STAGE_ROOT/$package_name"
    local executable_files=(
        tests/audit_windows_binary.sh
        packaging/build-release.sh
    )
    local regular_files=(
        BUILDING.md
        assets/outlast-requeue.ico
        assets/outlast-requeue.png
        docs/screenshot.png
        src/common/requeue_engine.c
        src/common/requeue_engine.h
        src/windows/Makefile
        src/windows/outlast_requeue.manifest
        src/windows/outlast_requeue.rc
        src/windows/outlast_requeue_windows.c
        src/windows/resource.h
        tests/Makefile
        tests/fixtures/appmanifest_1304930.acf
        tests/fixtures/invasion_timeout.log
        tests/test_requeue_engine.c
        packaging/CHANGELOG.md
        packaging/LICENSE-CODE
        packaging/NEXUS_DESCRIPTION.md
        packaging/README.md
        packaging/RELEASE_CHECKLIST.md
        packaging/THIRD_PARTY_NOTICES.md
        packaging/compatibility.json
        packaging/windows/README.md
    )
    local relative

    mkdir -p -- "$destination"
    for relative in "${executable_files[@]}"; do
        copy_source_file "$relative" 0755 "$destination"
    done
    for relative in "${regular_files[@]}"; do
        copy_source_file "$relative" 0644 "$destination"
    done
    install -m 0644 "$ROOT_DIR/README.md" "$destination/README.md"

    assert_source_payload "$destination"
    assert_clean_payload "$destination"
    write_tree_checksums "$destination"
    make_zip "$STAGE_ROOT" "$package_name"
}

require_command find
require_command file
require_command install
require_command "$MAKE"
require_command sha256sum
require_command sort
require_command strings
require_command xargs
if command -v zip >/dev/null 2>&1; then
    PYTHON=""
elif command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    die "required command not found: zip or python"
fi

case "$VERSION" in
    *[!0-9A-Za-z._-]*|'')
        die "VERSION contains unsupported characters: $VERSION"
        ;;
esac

for requested in $PLATFORMS; do
    case "$requested" in
        windows|source) ;;
        *) die "unknown platform in PLATFORMS: $requested" ;;
    esac
done

[[ -n "${PLATFORMS//[[:space:]]/}" ]] || die "PLATFORMS cannot be empty"

grep -qE '"version":[[:space:]]*"'"$VERSION"'"' "$SCRIPT_DIR/compatibility.json" \
    || die "compatibility.json does not describe version $VERSION"
grep -qE '"last_verified_build_id":[[:space:]]*"'"$EXPECTED_BUILD_ID"'"' \
    "$SCRIPT_DIR/compatibility.json" \
    || die "compatibility.json does not describe game build $EXPECTED_BUILD_ID"
verify_release_constants

TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/outlast-requeue-release.XXXXXX")"
trap 'rm -rf -- "$TEMP_ROOT"' EXIT
STAGE_ROOT="$TEMP_ROOT/stage"
mkdir -p -- "$STAGE_ROOT"

# shellcheck disable=SC2086
"$MAKE" -C "$ROOT_DIR/tests" clean check $MAKE_ARGS
wants_platform windows && build_and_verify_windows

mkdir -p -- "$DIST_DIR"

wants_platform windows && build_windows_archive
wants_platform source && build_source_archive

(
    cd -- "$DIST_DIR"
    for archive in "${CREATED_ARCHIVES[@]}"; do
        sha256sum "$(basename -- "$archive")"
    done | LC_ALL=C sort
) > "$DIST_DIR/SHA256SUMS-$VERSION"

echo "release checksums: $DIST_DIR/SHA256SUMS-$VERSION"
