# Langegen catalog mirror

The built-in Langegen catalog refresh tries GitHub Raw, then the GitHub Contents
API with its raw media type. If both fail or return an invalid catalog, it loads
a previously published snapshot from the public GitLab Generic Package Registry
of `pipensx-group/pipensx` (project `88330126`). Custom catalog URLs and the
deprecated pipensx-catalog channel do not use these fallbacks.

All eight supported catalog languages are mirrored, together with
`catalog_diff.json`. Incremental refresh tries the live diff through Raw and the
API. If neither works, the app downloads a full catalog; it does not apply an
older mirrored diff to a potentially newer cache.

## Snapshot publication

`scripts/mirror_langegen.py` resolves an upstream commit and downloads every
file from that commit. It validates the catalog arrays and diff shape, computes
SHA-256, and compresses each file with zstd, including its decompressed size.
No catalogs are committed to this application's repository.

Snapshots use these package URLs:

```text
https://gitlab.com/api/v4/projects/88330126/packages/generic/langegen/<sha256>/<filename>.zst
https://gitlab.com/api/v4/projects/88330126/packages/generic/langegen/latest/manifest.json
```

The manifest maps upstream filenames to their uncompressed byte sizes and
SHA-256 hashes. It is uploaded last, after all referenced snapshots are
available. Failed upstream fetches, validation, or snapshot uploads leave the
previous manifest active. Unchanged files reuse existing snapshot packages;
an unchanged manifest is not uploaded again. GitLab serves the most recently
uploaded duplicate manifest file for the `latest` version.

The app constructs snapshot URLs itself, accepts only the expected project,
package, lowercase SHA-256 and filename, limits decompression to 48 MiB, and
verifies the decompressed size and SHA-256 before parsing or replacing its
cache. The manifest and files are trusted through GitLab HTTPS; checksums detect
corruption and mismatched snapshots, rather than providing an independent
signature of the upstream publisher.

## Automation and manual runs

The `mirror_langegen` GitLab CI job runs in scheduled pipelines on the default
branch. Configure a pipeline schedule with ref `main` and cron
`17 */6 * * *` in UTC. These pipelines run only the mirror job; application
builds, Pages deployment and code scans run in the regular pipelines.
Uploads use the existing `CI_JOB_TOKEN`; no new
long-lived secret is required. The job's resource group serializes publications.
Use the schedule's Run action to refresh through CI outside the regular times.

For a local inspection without publication (requires Python 3 and `zstd`):

```sh
python3 scripts/mirror_langegen.py --output /tmp/langegen-snapshot
python3 tests/test_catalog_mirror.py
```

To refresh the public mirror locally, authenticate `glab` to GitLab and run:

```sh
python3 scripts/mirror_langegen.py --publish
```

Keep the manifest and snapshots it references when maintaining registry
storage. A GitHub outage stops mirror updates but does not expire the last
published snapshot. A simultaneous GitHub/GitLab outage leaves the last valid
on-device cache available.
