# Custom RGW Sync Image Releases

Every pushed custom RGW sync image must have a matching GitHub Release and a
Slack announcement. The GitHub Release is the durable source of truth; Slack
contains a concise notification and link.

Do not push these images with `docker push` directly. Use the release wrapper:

```bash
export SLACK_BOT_TOKEN="..."
gh auth status

container/release-rgw-sync-debug.sh \
  --tag v20.2.1-short-description-YYYYMMDD-$(git rev-parse --short=12 HEAD)
```

The wrapper:

1. rejects `latest` and requires a named immutable tag;
2. builds and pushes the image;
3. records the registry digest and source commit in a GitHub Release;
4. generates the commit log and diffstat since the previous published image;
5. posts the changelog link to Slack channel `C085S5YF7DJ`.

The build publication records the runtime outcome as pending. After a canary or
deployment, record a measured result:

```bash
container/rgw-sync-image-release.sh result \
  --tag v20.2.1-short-description-YYYYMMDD-abcdef123456 \
  --event-id da1-dev-canary-1 \
  --scope "DA1-dev NoSuchBucket canary" \
  --result "Retry entry cleared and shard marker advanced" \
  --improved yes \
  --evidence "Grafana URL, job name, marker timestamps, and error counts"
```

Use a stable, unique `--event-id`. Repeating an event ID is a no-op, preventing
duplicate deployment-result announcements. Valid improvement values are `yes`,
`no`, and `pending`.

For an image that was pushed outside the wrapper, backfill it with:

```bash
container/rgw-sync-image-release.sh publish \
  --image registry.internal.telnyx.com/playground/ceph-rgw-custom:TAG \
  --digest sha256:DIGEST \
  --commit COMMIT \
  --previous-commit PREVIOUS_IMAGE_COMMIT
```

## Required Access

- `gh` authenticated with release-write access to `team-telnyx/ceph`;
- `SLACK_BOT_TOKEN` for a bot that can post to `C085S5YF7DJ`;
- authenticated Docker access to `registry.internal.telnyx.com`.

Never put credentials in command arguments, release notes, Git commits, or image
labels.
