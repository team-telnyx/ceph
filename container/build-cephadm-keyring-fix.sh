#!/usr/bin/env bash
# Build the ceph 20.2.4 + PR 72033 image (see Containerfile.cephadm-keyring-fix).
#
# The cephadm zipapp is built the way src/cephadm/CMakeLists.txt builds it
# (build.py, pip-bundled deps, the 20.2.4 version vars), inside the official
# base image so the Python is the same 3.9 that byte-compiled the official
# binary. The RPM build rewrites the zipapp shebang to "#! /usr/bin/python3 -s"
# (brp-mangle-shebangs); this script does the same.
#
# Usage: container/build-cephadm-keyring-fix.sh IMAGE[:TAG] [--push]
# Run from the repository root. Needs docker with linux/amd64 support.
set -euo pipefail

readonly BASE_IMAGE=quay.io/ceph/ceph@sha256:6bb1c8a42fbc0bf87938946990b65174466997bc11c31eb5a323225a779fd8f9

image=${1:?usage: $0 IMAGE[:TAG] [--push]}
push=${2:-}
stage=${BUILD_DIR:-build.cephadm-keyring-fix}
commit=$(git rev-parse HEAD)

[[ -z $(git status --porcelain -- src/cephadm src/python-common src/pybind/mgr/cephadm) ]] ||
  { echo "uncommitted changes under src/; commit first" >&2; exit 1; }

mkdir -p "$stage"
stage=$(cd "$stage" && pwd)
mkdir -p "$stage/src-$commit"
git archive HEAD src/cephadm src/python-common | tar -x -C "$stage/src-$commit"

docker run --rm --platform linux/amd64 --entrypoint bash \
  -v "$stage/src-$commit:/src:ro" -v "$stage:/out" "$BASE_IMAGE" -c '
    set -euo pipefail
    cp -a /src /tmp/build && cd /tmp/build/src/cephadm
    python3 build.py \
      --set-version-var=CEPH_GIT_VER=7f793731f1b39eb4f465e960113d2363c311b964 \
      --set-version-var=CEPH_GIT_NICE_VER=20.2.4 \
      --set-version-var=CEPH_RELEASE=20 \
      --set-version-var=CEPH_RELEASE_NAME=tentacle \
      --set-version-var=CEPH_RELEASE_TYPE=stable \
      --bundled-dependencies=pip \
      /out/cephadm.zipapp'

python3 - "$stage/cephadm.zipapp" "$stage/cephadm" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
nl = data.index(b'\n')
assert data[:nl] == b'#!/usr/bin/python3', data[:nl]
open(sys.argv[2], 'wb').write(b'#! /usr/bin/python3 -s' + data[nl:])
EOF

cp src/pybind/mgr/cephadm/serve.py src/pybind/mgr/cephadm/tests/test_cephadm.py "$stage/"

echo "cephadm sha256 (host-side name cephadm.<sha256>):"
shasum -a 256 "$stage/cephadm" 2>/dev/null || sha256sum "$stage/cephadm"

docker build --platform linux/amd64 --provenance=false \
  --build-arg BASE_IMAGE="$BASE_IMAGE" --build-arg SOURCE_COMMIT="$commit" \
  -f container/Containerfile.cephadm-keyring-fix -t "$image" "$stage"

if [[ $push == --push ]]; then
  docker push "$image"
fi
