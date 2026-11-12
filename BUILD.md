# Build instructions

## Source checkout

Clone with the pinned submodules, or initialize them in an existing checkout:

```bash
git clone --recurse-submodules https://gitlab.com/pipensx-group/pipensx.git
cd pipensx
# Existing clone only:
git submodule update --init --recursive
```

Run `make` to list the supported top-level targets.

### Development remotes

Gitea is the primary repository. For development with access to the local Gitea
server, configure a fresh GitLab clone as follows:

```bash
git remote rename origin gitlab
git remote add origin git@192.168.1.146:i3sey/pipensx.git
git fetch origin
git branch --set-upstream-to=origin/main main
git config remote.pushDefault origin
git config push.default simple
```

Create development branches from `main` and push them with
`git push -u origin <branch>`. Gitea pushes branches and tags to GitLab on each
push and every eight hours. Make changes on Gitea so the next mirror update can
copy them to GitLab. Repository mirroring does not copy issues or release assets.
Keep access tokens out of remote URLs; use SSH or a Git credential helper.

## PC client and tests

On Debian or Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential python3 libcurl4-openssl-dev libssl-dev \
  libzstd-dev zlib1g-dev
```

Build the portable command-line client or run the complete test suite:

```bash
make pc
make test
```

The client is written to `./pipensx` and accepts:

```bash
./pipensx /path/to/file.torrent [output_dir]
```

## Nintendo Switch

Requirements:

- devkitPro with devkitA64 and libnx
- CMake 3.13 or newer
- `switch-curl`, `switch-mbedtls`, `switch-zlib`, and `switch-miniupnpc`

Install them with devkitPro pacman:

```bash
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls \
  switch-zlib switch-miniupnpc
```

Build the release NRO:

```bash
export DEVKITPRO=/opt/devkitpro
make switch
```

The artifact is `build-switch/pipensx.nro`. Override CMake when it is not on
`PATH`:

```bash
make switch CMAKE_BIN=/path/to/cmake
```

Public builds contain no bundled catalog or game metadata dataset. At runtime,
catalog refresh also fetches the verified optional metadata index from the
configured pipensx-metadata manifest; failures keep the previous cache and do
not block catalog updates. A builder who needs a first-run offline
fallback may still embed a lawfully obtained compatible index explicitly:

```bash
make switch PIPENSX_METADATA_INDEX=/absolute/path/game_metadata_index.json
```

The path is build input only. Do not copy or commit the dataset under
`resources/catalog/`; that directory is ignored deliberately.

## Deploy over MTP

The deployment helper requires `gio` and an explicit target. By default it
replaces only the NRO:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/pipensx'
```

Set `DEPLOY_CLEAN=1` to remove other remote files while preserving cached
artwork under `catalog/images/`:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/pipensx' DEPLOY_CLEAN=1
```

## Cleanup and secret audit

```bash
make clean
make audit  # requires gitleaks in PATH
```

## CI

Pushes to `main` and pull requests run quick source-list, native/web translation,
and secret checks before compiling. PC changes build the CLI and run `make test`;
Switch source, build-script and resource changes build an NRO retained for seven
days. Documentation-only changes skip compilation. Both builds cache compiler
results with ccache; object files are rebuilt normally so header changes are safe.
ASan/UBSan and fuzz smoke run weekly on Monday and on manual `ci` dispatch.
Manual and weekly runs also check all builds and scan the complete Git history;
ordinary pushes scan only the new commits (unknown bases fall back to full checks).

GitHub uses `.github/workflows/ci.yml`; Gitea uses `.gitea/workflows/ci.yml`.
Keep their check selection and build steps aligned. The Gitea 1.22 workflow uses
v3 cache/artifact actions and the private `i3sey/borealis` dependency mirror,
pinned to the same submodule commit. `SUBMODULE_READ_TOKEN` is a repository
Actions secret with read-only repository access. When updating the borealis
submodule, push its commit to that mirror before pushing pipensx. Other
submodules use the URLs in `.gitmodules`. Outside Gitea CI, borealis uses the
GitLab mirror configured in `.gitmodules`.

GitLab uses `.gitlab-ci.yml` for PC and browser tests, Secret Detection, SAST,
and Pages on `main`. Tags and manual web/API pipelines also build the Switch
application after the tests pass. Only tags publish a release through the
GitLab Package Registry, with the NRO, its checksum, `SDOut.zip`, and the notes
from `releases/<version>-notes.md`. A tag build requires `VERSION` to match the
tag after removing an optional leading `v`. Manual builds do not publish a
release. The GitHub draft-release workflow remains in
`.github/workflows/switch-release.yml`.
