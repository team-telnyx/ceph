#!/usr/bin/env bash
set -euo pipefail

readonly DEFAULT_REGISTRY="registry.internal.telnyx.com/playground/ceph-rgw-custom"

usage() {
  cat <<'EOF'
Build, push, and publish an immutable custom RGW sync image.

Usage:
  release-rgw-sync-debug.sh --tag TAG [options]

Options:
  --tag TAG                Immutable image tag (required; 'latest' is forbidden)
  --registry REPOSITORY    Image repository
  --build-dir DIRECTORY    Ceph build directory (default: build.rgw-sync)
  --previous-commit SHA    Override previous published image commit
  --dry-run                Print docker and publication commands only

This command deliberately couples a registry push to GitHub and Slack changelog
publication. Build radosgw and radosgw-admin before invoking it.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

main() {
  local tag='' registry=$DEFAULT_REGISTRY build_dir=${BUILD_DIR:-build.rgw-sync}
  local previous_commit='' dry_run=false image push_output digest

  while (($#)); do
    case "$1" in
      --tag) tag=${2:?}; shift 2 ;;
      --registry) registry=${2:?}; shift 2 ;;
      --build-dir) build_dir=${2:?}; shift 2 ;;
      --previous-commit) previous_commit=${2:?}; shift 2 ;;
      --dry-run) dry_run=true; shift ;;
      -h|--help) usage; return 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done

  [[ -n "$tag" ]] || die "--tag is required"
  [[ "$tag" != latest ]] || die "the mutable tag 'latest' is forbidden"
  image="${registry}:${tag}"

  if [[ "$dry_run" == true ]]; then
    printf 'docker build --build-arg BUILD_DIR=%q -f container/Containerfile.rgw-sync-debug -t %q .\n' "$build_dir" "$image"
    printf 'docker push %q\n' "$image"
    printf 'container/rgw-sync-image-release.sh publish --image %q --digest sha256:<from-push>' "$image"
    [[ -n "$previous_commit" ]] && printf ' --previous-commit %q' "$previous_commit"
    printf '\n'
    return
  fi

  [[ -x "${build_dir}/bin/radosgw" ]] || die "missing ${build_dir}/bin/radosgw"
  [[ -x "${build_dir}/bin/radosgw-admin" ]] || die "missing ${build_dir}/bin/radosgw-admin"

  docker build --build-arg "BUILD_DIR=${build_dir}" \
    -f container/Containerfile.rgw-sync-debug -t "$image" .
  push_output=$(docker push "$image" 2>&1 | tee /dev/stderr)
  digest=$(sed -n 's/.*digest: \(sha256:[0-9a-f]\{64\}\).*/\1/p' <<<"$push_output" | tail -1)
  [[ -n "$digest" ]] || die "could not extract immutable digest from docker push"

  args=(publish --image "$image" --digest "$digest")
  [[ -n "$previous_commit" ]] && args+=(--previous-commit "$previous_commit")
  container/rgw-sync-image-release.sh "${args[@]}"
}

main "$@"
