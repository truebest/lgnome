#!/usr/bin/env bash
set -euo pipefail

# Publishes the local `main` tree to the GitHub release remote as ONE new release commit
# on top of the previous release commit. Development history stays only in the Bitbucket
# repo (origin); the GitHub history is an append-only chain of release snapshots, one
# commit per version. GitHub history is NEVER rewritten: pushes are fast-forward only and
# existing tags are never moved.
#
# Usage:            tools/release-github.sh <version>        e.g. tools/release-github.sh 0.1.2
# Remote override:  RELEASE_REMOTE=<name> (default: github)
# Branch override:  RELEASE_BRANCH=<name> (default: main)
#
# This script only pushes the tag/commit. It does NOT create the GitHub Releases entry
# (Releases page, release notes, .ipk asset) — that's a separate manual step:
#   gh release create vX.Y.Z --repo truebest/lgnome --title "lgnome X.Y.Z" \
#     --notes "..." dist/native-webos/com.truebest.lgnome.native_X.Y.Z_arm.ipk

version="${1:?usage: tools/release-github.sh <version>   e.g. 0.1.2}"
remote="${RELEASE_REMOTE:-github}"
branch="${RELEASE_BRANCH:-main}"
tag="v${version}"

fail() {
  echo "release-github: $*" >&2
  exit 2
}

git diff --quiet && git diff --cached --quiet || fail "working tree is dirty; commit or stash first"
git rev-parse --verify main >/dev/null 2>&1 || fail "local main branch not found"
git remote get-url "$remote" >/dev/null 2>&1 || fail "remote '$remote' is not configured"
git rev-parse --verify --quiet "refs/tags/$tag" >/dev/null && fail "tag $tag already exists locally; bump the version"

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

parent_args=()
if git fetch --quiet "$remote" "$branch" 2>/dev/null; then
  parent="$(git rev-parse FETCH_HEAD)"
  parent_args=(-p "$parent")
  [ "$(git rev-parse "$parent^{tree}")" = "$tree" ] && fail "tree is identical to the latest published release; nothing to release"
fi

commit="$(git commit-tree "$tree" "${parent_args[@]}" -m "lgnome ${version}")"

git tag "$tag" "$commit" >/dev/null
git push "$remote" "$commit:refs/heads/$branch" "refs/tags/$tag"
echo "release-github: published release commit $commit as $remote/$branch ($tag)"
echo "release-github: tag pushed, but the GitHub Releases entry is NOT created yet — run:" >&2
echo "  gh release create $tag --repo truebest/lgnome --title \"lgnome $version\" --notes \"...\" dist/native-webos/com.truebest.lgnome.native_${version}_arm.ipk" >&2
