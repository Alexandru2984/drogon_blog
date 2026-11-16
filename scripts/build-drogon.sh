#!/usr/bin/env bash
# Build a private static Drogon; never replace a host-wide shared library.
set -euo pipefail
prefix=${1:-"${XDG_DATA_HOME:-$HOME/.local/share}/drogon-blog/drogon-1.9.14"}
revision=3aae769cc405cf5abb9d477675c6430924f02c7c
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

git clone --quiet --depth 1 --branch v1.9.14 \
    https://github.com/drogonframework/drogon.git "$build_dir/source"
test "$(git -C "$build_dir/source" rev-parse HEAD)" = "$revision"
git -C "$build_dir/source" submodule update --quiet --init --recursive --depth 1
cmake -S "$build_dir/source" -B "$build_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_DROGON_SHARED=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_CTL=ON
cmake --build "$build_dir/build" -j"${BLOG_BUILD_JOBS:-2}"
cmake --install "$build_dir/build"
printf 'Private static Drogon installed. Configure with -DCMAKE_PREFIX_PATH=%s\n' "$prefix"
