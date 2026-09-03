#!/usr/bin/env bash
set -euo pipefail

readonly DEFAULT_REPO="team-telnyx/ceph"
readonly DEFAULT_CHANNEL="C085S5YF7DJ"
readonly RELEASE_MARKER="<!-- rgw-sync-image-release -->"
TEMP_FILES=()

cleanup() {
  ((${#TEMP_FILES[@]} == 0)) || rm -f "${TEMP_FILES[@]}"
}

trap cleanup EXIT

usage() {
  cat <<'EOF'
Publish and update the changelog for an immutable custom RGW image.

Usage:
  rgw-sync-image-release.sh publish --image IMAGE --digest SHA256 [options]
  rgw-sync-image-release.sh result --tag TAG --event-id ID --scope TEXT \
    --result TEXT --improved yes|no|pending [--evidence TEXT] [options]

Publish options:
  --commit SHA             Image source commit (default: HEAD)
  --previous-commit SHA    Override the previous published image commit

Common options:
  --repo OWNER/REPO        GitHub repository (default: team-telnyx/ceph)
  --channel CHANNEL_ID     Slack channel (default: C085S5YF7DJ)
  --dry-run                Render output without publishing

Required environment:
  GH_TOKEN or authenticated gh CLI
  SLACK_BOT_TOKEN
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

append_line() {
  local file=$1
  shift
  printf '%s\n' "$*" >>"$file"
}

post_slack() {
  local channel=$1
  local message=$2
  local response

  [[ -n "${SLACK_BOT_TOKEN:-}" ]] || die "SLACK_BOT_TOKEN is required"
  response=$(curl --fail --silent --show-error \
    -X POST \
    -H "Authorization: Bearer ${SLACK_BOT_TOKEN}" \
    -H "Content-Type: application/json; charset=utf-8" \
    --data "$(jq -n --arg channel "$channel" --arg text "$message" \
      '{channel: $channel, text: $text}')" \
    https://slack.com/api/chat.postMessage)
  [[ $(jq -r '.ok' <<<"$response") == true ]] || \
    die "Slack publication failed: $(jq -r '.error // "unknown error"' <<<"$response")"
  jq -r '.ts' <<<"$response"
}

latest_release_json() {
  local repo=$1
  gh api --paginate "repos/${repo}/releases?per_page=100" | \
    jq -s --arg marker "$RELEASE_MARKER" \
      'add | map(select((.body // "") | contains($marker))) | sort_by(.published_at) | last // {}'
}

release_commit() {
  jq -r '.body // ""' | \
    sed -n 's/^<!-- rgw-sync-commit: \([0-9a-f]\{40\}\) -->$/\1/p' | \
    head -1
}

release_digest() {
  sed -n 's/^<!-- rgw-sync-digest: \(sha256:[0-9a-f]\{64\}\) -->$/\1/p' | head -1
}

render_changes() {
  local previous_commit=$1
  local commit=$2

  if [[ -n "$previous_commit" ]]; then
    git log --no-merges --format="- \`%h\` %s" "${previous_commit}..${commit}"
  else
    git show -s --format="- \`%h\` %s" "$commit"
  fi
}

render_diffstat() {
  local previous_commit=$1
  local commit=$2

  if [[ -n "$previous_commit" ]]; then
    git diff --stat "$previous_commit" "$commit"
  else
    git show --stat --format= "$commit"
  fi
}

publish_release() {
  local repo=$1 channel=$2 image=$3 digest=$4 commit=$5 previous_commit=$6 dry_run=$7
  local tag latest latest_tag latest_url notes changes diffstat release_url slack_ts existing body change_count
  local slack_message short_commit short_digest

  [[ "$image" == *:* ]] || die "image must include an immutable tag"
  tag=${image##*:}
  [[ "$tag" != latest ]] || die "the mutable tag 'latest' is forbidden"
  [[ "$tag" =~ ^[A-Za-z0-9][A-Za-z0-9._-]+$ ]] || die "invalid image tag: $tag"
  [[ "$digest" =~ ^sha256:[0-9a-f]{64}$ ]] || die "invalid image digest: $digest"

  commit=$(git rev-parse "${commit}^{commit}")
  if [[ -n "$previous_commit" ]]; then
    previous_commit=$(git rev-parse "${previous_commit}^{commit}")
  else
    latest=$(latest_release_json "$repo")
    latest_tag=$(jq -r '.tag_name // empty' <<<"$latest")
    latest_url=$(jq -r '.html_url // empty' <<<"$latest")
    previous_commit=$(release_commit <<<"$latest")
  fi

  if [[ -n "$previous_commit" ]]; then
    git merge-base --is-ancestor "$previous_commit" "$commit" || \
      die "previous image commit is not an ancestor of ${commit}"
  fi

  changes=$(render_changes "$previous_commit" "$commit")
  [[ -n "$changes" ]] || changes='- No source changes; image rebuild only.'
  diffstat=$(render_diffstat "$previous_commit" "$commit")
  [[ -n "$diffstat" ]] || diffstat='No file changes.'

  notes=$(mktemp)
  TEMP_FILES+=("$notes")
  cat >"$notes" <<EOF
${RELEASE_MARKER}
<!-- rgw-sync-commit: ${commit} -->
<!-- rgw-sync-digest: ${digest} -->

## Immutable Artifact

| Field | Value |
| --- | --- |
| Image | \`${image}\` |
| Digest | \`${digest}\` |
| Source | [\`${commit}\`](https://github.com/${repo}/commit/${commit}) |
| Runtime result | **Pending** |
EOF

  if [[ -n "$previous_commit" ]]; then
    append_line "$notes" "| Previous source | [\`${previous_commit}\`](https://github.com/${repo}/commit/${previous_commit}) |"
    if [[ -n "${latest_tag:-}" && -n "${latest_url:-}" ]]; then
      append_line "$notes" "| Previous release | [\`${latest_tag}\`](${latest_url}) |"
    fi
  else
    append_line "$notes" "| Previous release | Not recorded; this is the first ledger entry |"
  fi

  cat >>"$notes" <<EOF

## Changes Since Previous Immutable Image

${changes}

## Diff Summary

\`\`\`
${diffstat}
\`\`\`

## Validation History

### Build published

- Deployment: not started
- Outcome: runtime validation pending
- Improved sync: **pending**
EOF

  if [[ "$dry_run" == true ]]; then
    cat "$notes"
    return
  fi

  existing=$(gh release view "$tag" --repo "$repo" --json body,url 2>/dev/null || true)
  if [[ -n "$existing" ]]; then
    body=$(jq -r '.body' <<<"$existing")
    [[ $(release_digest <<<"$body") == "$digest" ]] || \
      die "GitHub release ${tag} already exists with a different digest"
    release_url=$(jq -r '.url' <<<"$existing")
    if grep -q '^<!-- rgw-sync-slack-build-ts:' <<<"$body"; then
      printf 'release already published: %s\n' "$release_url"
      return
    fi
    printf '%s\n' "$body" >"$notes"
    changes=$(sed -n '/^## Changes Since Previous Immutable Image$/,/^## Diff Summary$/p' <<<"$body")
  else
    gh release create "$tag" --repo "$repo" --target "$commit" \
      --title "RGW sync image ${tag}" --notes-file "$notes"
    release_url=$(gh release view "$tag" --repo "$repo" --json url --jq .url)
  fi

  change_count=$(grep -c '^- `' <<<"$changes" || true)
  short_commit=${commit:0:12}
  short_digest=${digest:0:19}
  slack_message=$(cat <<EOF
*RGW sync image published*
\`${tag}\`

*Artifact*
• Commit: \`${short_commit}\`
• Digest: \`${short_digest}…\`
• Delta: ${change_count} commit(s) since the previous immutable image

*Validation*
• Deployed: no
• Improved sync: *pending*

<${release_url}|Full changelog and immutable digest>
EOF
)
  slack_ts=$(post_slack "$channel" "$slack_message")
  append_line "$notes" "<!-- rgw-sync-slack-build-ts: ${slack_ts} -->"
  gh release edit "$tag" --repo "$repo" --notes-file "$notes" >/dev/null
  printf 'published %s and Slack message %s\n' "$release_url" "$slack_ts"
}

record_result() {
  local repo=$1 channel=$2 tag=$3 event_id=$4 scope=$5 result=$6 improved=$7 evidence=$8 dry_run=$9
  local release body release_url notes timestamp slack_ts event_recorded=false slack_message

  [[ "$event_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]+$ ]] || die "invalid event id: $event_id"
  [[ "$improved" =~ ^(yes|no|pending)$ ]] || die "--improved must be yes, no, or pending"
  release=$(gh release view "$tag" --repo "$repo" --json body,url 2>/dev/null) || \
    die "GitHub release not found: $tag"
  body=$(jq -r '.body' <<<"$release")
  release_url=$(jq -r '.url' <<<"$release")
  grep -qF "$RELEASE_MARKER" <<<"$body" || die "release is not an RGW image ledger entry"

  if grep -qF "<!-- rgw-sync-result-event: ${event_id} -->" <<<"$body"; then
    event_recorded=true
    if grep -qF "<!-- rgw-sync-slack-result-${event_id}-ts:" <<<"$body"; then
      printf 'result event already recorded and published: %s\n' "$event_id"
      return
    fi
  fi

  timestamp=$(date -u +'%Y-%m-%dT%H:%M:%SZ')
  notes=$(mktemp)
  TEMP_FILES+=("$notes")
  printf '%s\n' "$body" >"$notes"
  if [[ "$event_recorded" == false ]]; then
    cat >>"$notes" <<EOF

### ${timestamp}: ${scope}

<!-- rgw-sync-result-event: ${event_id} -->
- Outcome: ${result}
- Improved sync: **${improved}**
- Evidence: ${evidence:-not supplied}
EOF
  fi

  if [[ "$dry_run" == true ]]; then
    tail -8 "$notes"
    return
  fi

  if [[ "$event_recorded" == false ]]; then
    gh release edit "$tag" --repo "$repo" --notes-file "$notes" >/dev/null
  fi
  slack_message=$(cat <<EOF
*RGW sync image validation*
\`${tag}\`

*Scope*
${scope}

*Outcome*
${result}

*Improved sync:* ${improved}
<${release_url}|Full changelog and evidence>
EOF
)
  slack_ts=$(post_slack "$channel" "$slack_message")
  append_line "$notes" "<!-- rgw-sync-slack-result-${event_id}-ts: ${slack_ts} -->"
  gh release edit "$tag" --repo "$repo" --notes-file "$notes" >/dev/null
  printf 'recorded result in %s and Slack message %s\n' "$release_url" "$slack_ts"
}

main() {
  local action=${1:-}
  shift || true
  local repo=${RGW_SYNC_GITHUB_REPO:-$DEFAULT_REPO}
  local channel=${RGW_SYNC_SLACK_CHANNEL:-$DEFAULT_CHANNEL}
  local image='' digest='' commit=HEAD previous_commit='' tag='' event_id=''
  local scope='' result='' improved='' evidence=''
  local dry_run=false

  case "$action" in
    publish|result) ;;
    -h|--help|'') usage; return 0 ;;
    *) die "unknown action: $action" ;;
  esac

  while (($#)); do
    case "$1" in
      --repo) repo=${2:?}; shift 2 ;;
      --channel) channel=${2:?}; shift 2 ;;
      --image) image=${2:?}; shift 2 ;;
      --digest) digest=${2:?}; shift 2 ;;
      --commit) commit=${2:?}; shift 2 ;;
      --previous-commit) previous_commit=${2:?}; shift 2 ;;
      --tag) tag=${2:?}; shift 2 ;;
      --event-id) event_id=${2:?}; shift 2 ;;
      --scope) scope=${2:?}; shift 2 ;;
      --result) result=${2:?}; shift 2 ;;
      --improved) improved=${2:?}; shift 2 ;;
      --evidence) evidence=${2:?}; shift 2 ;;
      --dry-run) dry_run=true; shift ;;
      -h|--help) usage; return 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done

  require_command git
  require_command gh
  require_command jq
  require_command curl

  if [[ "$action" == publish ]]; then
    [[ -n "$image" && -n "$digest" ]] || die "publish requires --image and --digest"
    publish_release "$repo" "$channel" "$image" "$digest" "$commit" "$previous_commit" "$dry_run"
  else
    [[ -n "$tag" && -n "$event_id" && -n "$scope" && -n "$result" && -n "$improved" ]] || \
      die "result requires --tag, --event-id, --scope, --result, and --improved"
    record_result "$repo" "$channel" "$tag" "$event_id" "$scope" "$result" "$improved" "$evidence" "$dry_run"
  fi
}

main "$@"
