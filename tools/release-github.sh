#!/usr/bin/env bash
set -euo pipefail

# Prepare one cumulative release commit on the previous GitHub release.
# Push the candidate through a PR; tag the merged main commit only after CI and review.
# Usage: tools/release-github.sh <version> /path/to/verified.ipk

version="${1:?usage: tools/release-github.sh <version> /path/to/verified.ipk}"
ipk="${2:?usage: tools/release-github.sh <version> /path/to/verified.ipk}"
remote="${RELEASE_REMOTE:-github}"
branch="${RELEASE_BRANCH:-main}"
tag="v${version}"
candidate="release/$tag"

fail() {
  echo "release-github: $*" >&2
  exit 2
}

[[ $# == 2 && "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "expected a version X.Y.Z and a verified IPK"
[[ -f "$ipk" ]] || fail "IPK does not exist: $ipk"
git check-ref-format "refs/heads/$candidate" || fail "invalid candidate branch"
git diff --quiet && git diff --cached --quiet || fail "working tree is dirty; commit or stash first"
git rev-parse --verify main >/dev/null 2>&1 || fail "local main branch not found"
git remote get-url "$remote" >/dev/null 2>&1 || fail "remote '$remote' is not configured"
git rev-parse --verify --quiet "refs/tags/$tag" >/dev/null && fail "tag $tag already exists locally; bump the version"

git show-ref --verify --quiet "refs/heads/$candidate" && fail "candidate $candidate already exists"
remote_tags="$(git ls-remote --tags "$remote" "refs/tags/$tag")" || fail "cannot inspect release tags"
[[ -z "$remote_tags" ]] || fail "tag $tag already exists remotely; bump the version"

python3 - "$version" "$ipk" <<'PY_VERSION'
import io
import json
import subprocess
import sys
import tarfile

version, ipk = sys.argv[1:]
app_id = "com.truebest.lgnome.native"
source = json.loads(subprocess.check_output(["git", "show", "main:native/deploy/webos/appinfo.json"]))
members = subprocess.check_output(["ar", "t", ipk], text=True).splitlines()
payloads = [name for name in members if name.startswith("data.tar")]
if len(payloads) != 1:
    raise SystemExit("release-github: expected one IPK data payload")
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(["ar", "p", ipk, payloads[0]]))) as archive:
    paths = [entry for entry in archive if entry.name.lstrip("./") ==
             f"usr/palm/applications/{app_id}/appinfo.json" and entry.isfile()]
    if len(paths) != 1:
        raise SystemExit("release-github: missing or ambiguous IPK appinfo.json")
    packaged = json.load(archive.extractfile(paths[0]))
if any(info.get("id") != app_id or info.get("version") != version for info in (source, packaged)):
    raise SystemExit("release-github: requested version/identity differs from main or IPK appinfo.json")
PY_VERSION
"$(dirname "${BASH_SOURCE[0]}")/build-native-webos.sh" --verify-ipk "$ipk"

tree="$(git rev-parse "main^{tree}")"

# backend_ndl development pins live in the private Bitbucket repo; the public release
# mirror is an append-only snapshot chain, so a dev gitlink can never resolve there.
# Rewrite the released tree to reference the mirror, and require the mirror's latest
# snapshot to carry the exact tree of the dev pin so a release never ships submodule
# content that differs from what was built and tested.
ndl_mirror_url="${BACKEND_NDL_MIRROR_URL:-https://github.com/truebest/backend_ndl.git}"
ndl_dev_url="git@bitbucket.org:kodavr/backend_ndl.git"
ndl_pin="$(git rev-parse "main:third_party/backend_ndl")"
git -C third_party/backend_ndl fetch --quiet "$ndl_mirror_url" main ||
  fail "cannot fetch the backend_ndl mirror $ndl_mirror_url"
ndl_mirror_head="$(git -C third_party/backend_ndl rev-parse FETCH_HEAD)"
ndl_pin_tree="$(git -C third_party/backend_ndl rev-parse "$ndl_pin^{tree}" 2>/dev/null)" ||
  fail "backend_ndl dev pin $ndl_pin is not present in the submodule checkout; git submodule update first"
[ "$(git -C third_party/backend_ndl rev-parse "FETCH_HEAD^{tree}")" = "$ndl_pin_tree" ] ||
  fail "backend_ndl mirror main ($ndl_mirror_head) does not match the dev pin ($ndl_pin); publish the backend_ndl release first"
gitmodules_blob="$(git show "main:.gitmodules" | sed "s|$ndl_dev_url|$ndl_mirror_url|" | git hash-object -w --stdin)"
git cat-file blob "$gitmodules_blob" | grep -Fq "$ndl_mirror_url" ||
  fail ".gitmodules rewrite produced no mirror URL; check the $ndl_dev_url pattern"
# A not-yet-existing index path: handing git a pre-created zero-length index file is
# accepted by current git but has been rejected as corrupt by other versions.
release_scratch="$(mktemp -d)"
trap 'rm -rf "$release_scratch"' EXIT
release_index="$release_scratch/index"
GIT_INDEX_FILE="$release_index" git read-tree "$tree"
GIT_INDEX_FILE="$release_index" git update-index --cacheinfo "100644,$gitmodules_blob,.gitmodules"
GIT_INDEX_FILE="$release_index" git update-index --cacheinfo "160000,$ndl_mirror_head,third_party/backend_ndl"
tree="$(GIT_INDEX_FILE="$release_index" git write-tree)"

git fetch --quiet "$remote" "$branch" || fail "cannot fetch the previous release branch"
parent="$(git rev-parse FETCH_HEAD)"
[ "$(git rev-parse "$parent^{tree}")" = "$tree" ] && fail "tree is identical to the latest published release; nothing to release"
commit="$(git commit-tree "$tree" -p "$parent" -m "lgnome ${version}")"
git branch "$candidate" "$commit"
echo "release-github: prepared $candidate ($commit), parent $parent"
echo "release-github: verified IPK SHA-256: $(sha256sum "$ipk" | cut -d ' ' -f 1)"
echo "release-github: push $candidate to $remote with an explicit refspec, open a PR to $branch, and wait for CI and review of the current head."
echo "release-github: after squash merge, tag the resulting $branch commit $tag and create the Release with this IPK."
