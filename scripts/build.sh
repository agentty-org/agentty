#!/bin/sh
# scripts/build.sh — the safe way to build agentty.
#
#   scripts/build.sh                      # configure (debug preset) if needed, build at -j12
#   scripts/build.sh --preset release     # (re)configure as release, then build
#   scripts/build.sh -- --target agentty  # args after -- go to `cmake --build`
#   scripts/build.sh -- -j8               # an explicit -j / --parallel wins
#   AGENTTY_BUILD_JOBS=16 scripts/build.sh
#
# What it adds over a bare `cmake --build build`:
#   * -j12 by default (AGENTTY_BUILD_JOBS overrides). Per-kind concurrency is
#     still capped by the memory-aware Ninja pools set at configure time, so
#     a smaller box can't be pushed into swap. Raise the compile pool with
#     -DAGENTTY_COMPILE_JOBS=12 if RAM sizing put it below the -j.
#   * One build per tree. It takes an exclusive lock on build/.agentty-build.lock,
#     so a second build of the same tree (another terminal, another agent
#     session, an IDE) WAITS instead of running two ninjas in one tree. Two
#     ninjas tear .ninja_log/.ninja_deps, and ninja then recompiles work
#     already done.
#   * Self-healing: logs torn by an interrupted build are recompacted first.
#   * Fresh clones just work: configure fetches missing submodules
#     (AGENTTY_AUTO_INIT_SUBMODULES).
#   * AGENTTY_BUILD_LOCK=/path/file shares ONE lock across build trees.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preset=debug
explicit_preset=0
build_dir="$root/build"

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset=$2; explicit_preset=1; shift 2 ;;
        --preset=*) preset=${1#*=}; explicit_preset=1; shift ;;
        -B) build_dir=$2; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) break ;;
    esac
done

case "$build_dir" in
    /*) ;;
    *) build_dir="$PWD/$build_dir" ;;
esac

# Default parallelism: -j12 unless the caller passed -j/--parallel themselves.
has_jobs=0
for a in "$@"; do
    case "$a" in
        -j|-j*|--parallel|--parallel=*) has_jobs=1 ;;
    esac
done
if [ "$has_jobs" = 0 ]; then
    set -- "$@" -j "${AGENTTY_BUILD_JOBS:-12}"
fi

mkdir -p "$build_dir"
# Per-tree by default. Point AGENTTY_BUILD_LOCK at one shared file to
# serialize builds of DIFFERENT trees too (e.g. build/ and build-mcp/ from
# parallel agents), so two -j12 builds never run at once and exhaust RAM.
lock="${AGENTTY_BUILD_LOCK:-$build_dir/.agentty-build.lock}"

configure() {
    cd "$root"
    # Configure on first use, or whenever a preset is asked for by name
    # (`--preset release` after a debug build flips the tree in place).
    if [ "$explicit_preset" = 1 ] || { [ ! -f "$build_dir/build.ninja" ] && [ ! -f "$build_dir/Makefile" ]; }; then
        cmake --preset "$preset" -B "$build_dir"
    fi
    heal
}

# A ninja killed mid-write (OOM killer, Ctrl-C at the wrong moment, an agent
# timeout) or two ninjas in one tree tear .ninja_deps / .ninja_log. When ninja
# finds the tear it truncates the file there and warns "premature end of file;
# recovering". The tear stays in place, though, so every record appended after
# it is lost again on the next load, and the same objects rebuild on every
# run. `ninja -t recompact` rewrites both files cleanly. It runs only when a
# tear is detected, and it's safe because we hold the tree lock.
heal() {
    [ -f "$build_dir/build.ninja" ] || return 0
    command -v ninja >/dev/null 2>&1 || return 0
    if ninja -C "$build_dir" -t recompact 2>&1 | grep -q 'premature end of file'; then
        echo "agentty: repaired torn ninja logs in $build_dir (an earlier build was interrupted)" >&2
    fi
}

if command -v flock >/dev/null 2>&1; then
    exec 9>"$lock"
    if ! flock -n 9; then
        echo "agentty: another build holds $build_dir — waiting for it to finish…" >&2
        flock 9
    fi
    configure
    # fd 9 is inherited across exec, so the lock lasts as long as the build.
    exec cmake --build "$build_dir" "$@"
else
    # macOS has no flock(1): fall back to an atomic mkdir lock.
    ldir="$lock.d"
    while ! mkdir "$ldir" 2>/dev/null; do
        echo "agentty: another build holds $build_dir — waiting (stale? rmdir $ldir)…" >&2
        sleep 2
    done
    trap 'rmdir "$ldir"' EXIT INT TERM
    configure
    cmake --build "$build_dir" "$@"
fi
