#!/usr/bin/env bash
set -euo pipefail

git config --global --add safe.directory /ceph

build_dir="${BUILD_DIR:-build}"

./do_cmake.sh \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_TESTS=OFF \
  -DWITH_MGR_DASHBOARD_FRONTEND=OFF \
  -DWITH_SYSTEMD=OFF

ninja -C "${build_dir}" radosgw
