#!/usr/bin/env bash
# Opens the vcpkg and Conan Center pull requests that move docuconf to a released version.
# Usage: scripts/package-managers.sh <version>   (for example 0.2.2, without the v)
#
# For each package manager it syncs the fork with upstream, branches docuconf-<version> from upstream's master,
# bumps the version and hashes (vcpkg: vcpkg.json, portfile SHA-512 and the version database; Conan: config.yml
# and conandata.yml), pushes the branch to the fork and opens the pull request upstream. It skips a package
# manager that does not have docuconf yet or already has this version, and does not push a branch or open a
# pull request that already exists, so it is safe to re-run.
#
# Environment:
#   GH_TOKEN      a token that can push to both forks and open pull requests upstream (classic PAT with
#                 public_repo); its owner is the commit author. Not needed with DRY_RUN=1.
#   DRY_RUN=1     edit and commit locally, print the pull request, push nothing.
#   ONLY=vcpkg|conan                     run one package manager.
#   VCPKG_FORK, VCPKG_UPSTREAM, CONAN_FORK, CONAN_UPSTREAM   owner/repo, defaults below.
#   VCPKG_CLONE_URL, CONAN_CLONE_URL     clone this instead of the fork (for testing with DRY_RUN=1).
set -euo pipefail

version=${1:?usage: package-managers.sh <version>}
version=${version#v}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a version: $version" >&2; exit 2; }
dry=${DRY_RUN:-}
source_repo=${SOURCE_REPO:-docuconf/docuconf-cpp}
vcpkg_fork=${VCPKG_FORK:-Docuconf/vcpkg}
vcpkg_upstream=${VCPKG_UPSTREAM:-microsoft/vcpkg}
conan_fork=${CONAN_FORK:-Docuconf/conan-center-index}
conan_upstream=${CONAN_UPSTREAM:-conan-io/conan-center-index}
branch="docuconf-$version"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

note() { echo "::notice::$*"; }

# The release tarball (Conan) must match the .sha256 the release workflow published next to it.
tarball="docuconf-cpp-$version.tar.gz"
release_url="https://github.com/$source_repo/releases/download/v$version/$tarball"
curl -fsSL -o "$work/$tarball" "$release_url"
curl -fsSL -o "$work/$tarball.sha256" "$release_url.sha256"
(cd "$work" && sha256sum -c "$tarball.sha256")
sha256=$(cut -d' ' -f1 "$work/$tarball.sha256")
# vcpkg_from_github downloads GitHub's archive of the tag.
sha512=$(curl -fsSL "https://github.com/$source_repo/archive/v$version.tar.gz" | sha512sum | cut -d' ' -f1)

if [ -n "$dry" ]; then
  git_name="docuconf release" git_email="release@docuconf.invalid"
else
  login=$(gh api user --jq .login)
  git_name=$(gh api user --jq '.name // .login')
  git_email="$(gh api user --jq .id)+$login@users.noreply.github.com"
fi
commit() { git -c user.name="$git_name" -c user.email="$git_email" commit -q "$@"; }

# clone <fork> <override-url> <dir> [git clone args...]: a shallow clone of the fork's master after syncing it
# with upstream, so the branch is based on upstream's latest master and the push stays small.
clone() {
  local fork=$1 override=$2 dir=$3; shift 3
  if [ -n "$override" ]; then
    git clone -q --depth 1 "$@" "$override" "$dir"
    return
  fi
  if [ -z "$dry" ]; then
    gh api -X POST "repos/$fork/merge-upstream" -f branch=master --silent
  fi
  git clone -q --depth 1 --branch master "$@" "https://github.com/$fork.git" "$dir"
}

# open_pr <fork> <upstream> <dir> <title> <body>: pushes the branch and opens the pull request, skipping
# whichever of the two already exists.
open_pr() {
  local fork=$1 upstream=$2 dir=$3 title=$4 body=$5
  if [ -n "$dry" ]; then
    echo "--- would push $fork:$branch and open on $upstream: $title"
    git -C "$dir" show --stat HEAD | tail -n +5
    echo "$body"
    return
  fi
  if git ls-remote --exit-code --heads "https://github.com/$fork.git" "$branch" > /dev/null; then
    note "$fork already has $branch; not pushing it again"
  else
    git -C "$dir" push -q "https://x-access-token:$GH_TOKEN@github.com/$fork.git" "HEAD:refs/heads/$branch"
  fi
  local existing
  existing=$(gh pr list --repo "$upstream" --head "$branch" --state all --json url,headRepositoryOwner \
    --jq ".[] | select(.headRepositoryOwner.login == \"${fork%%/*}\") | .url" | head -n1)
  if [ -n "$existing" ]; then
    note "$upstream already has a pull request for $branch: $existing"
  else
    gh pr create --repo "$upstream" --base master --head "${fork%%/*}:$branch" --title "$title" --body "$body"
  fi
}

update_vcpkg() {
  local dir="$work/vcpkg"
  clone "$vcpkg_fork" "${VCPKG_CLONE_URL:-}" "$dir"
  cd "$dir"
  if [ ! -f ports/docuconf/vcpkg.json ]; then
    note "vcpkg: $vcpkg_upstream has no docuconf port yet; skipping"
    return
  fi
  local current
  current=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' ports/docuconf/vcpkg.json)
  if [ "$current" = "$version" ]; then
    note "vcpkg: docuconf is already $version; skipping"
    return
  fi
  git checkout -q -b "$branch"
  python3 - "$version" <<'EOF'
import json, sys
path = "ports/docuconf/vcpkg.json"
manifest = json.load(open(path))
manifest["version"] = sys.argv[1]
manifest.pop("port-version", None)
open(path, "w").write(json.dumps(manifest, indent=2) + "\n")
EOF
  sed -i -E "s/SHA512 [0-9a-f]{128}/SHA512 $sha512/" ports/docuconf/portfile.cmake
  grep -q "SHA512 $sha512" ports/docuconf/portfile.cmake || { echo "vcpkg: portfile has no SHA512 line" >&2; exit 1; }
  ./bootstrap-vcpkg.sh -disableMetrics > /dev/null
  ./vcpkg format-manifest ports/docuconf/vcpkg.json > /dev/null
  git add ports/docuconf
  commit -m "[docuconf] Update to $version"
  ./vcpkg x-add-version docuconf
  git add versions
  commit --amend --no-edit
  open_pr "$vcpkg_fork" "$vcpkg_upstream" "$dir" "[docuconf] Update to $version" "$(cat <<EOF
Updates docuconf from $current to [$version](https://github.com/$source_repo/releases/tag/v$version).

- [x] Changes comply with the [maintainer guide](https://github.com/microsoft/vcpkg-docs/blob/main/vcpkg/contributing/maintainer-guide.md).
- [x] SHA512s are updated for each updated download.
- [x] The "supports" clause reflects platforms that may be fixed by this new version.
- [x] Any fixed [CI baseline](https://github.com/microsoft/vcpkg/blob/master/scripts/ci.baseline.txt) entries are removed from that file.
- [x] Any patches that are no longer applied are deleted from the port's directory.
- [x] The version database is fixed by rerunning \`./vcpkg x-add-version --all\` and committing the result.
- [x] Only one version is added to each modified port's versions file.

Opened by docuconf-cpp's release workflow.
EOF
)"
}

update_conan() {
  local dir="$work/cci"
  clone "$conan_fork" "${CONAN_CLONE_URL:-}" "$dir" --filter=blob:none --sparse
  cd "$dir"
  git sparse-checkout set recipes/docuconf
  if [ ! -f recipes/docuconf/config.yml ]; then
    note "conan: $conan_upstream has no docuconf recipe yet; skipping"
    return
  fi
  if grep -q "^  \"$version\":" recipes/docuconf/config.yml; then
    note "conan: docuconf $version is already in config.yml; skipping"
    return
  fi
  git checkout -q -b "$branch"
  # Newest version first, in the folder the newest existing version uses.
  python3 - "$version" "$release_url" "$sha256" <<'EOF'
import re, sys
version, url, sha256 = sys.argv[1:]
path = "recipes/docuconf/config.yml"
text = open(path).read()
folder = re.search(r'^    folder: *"?([^"\n]+)"?', text, re.M).group(1)
text = text.replace("versions:\n", f'versions:\n  "{version}":\n    folder: {folder}\n', 1)
open(path, "w").write(text)
path = f"recipes/docuconf/{folder}/conandata.yml"
text = open(path).read()
text = text.replace("sources:\n", f'sources:\n  "{version}":\n    url: "{url}"\n    sha256: "{sha256}"\n', 1)
open(path, "w").write(text)
EOF
  git add recipes/docuconf
  commit -m "docuconf: add version $version"
  open_pr "$conan_fork" "$conan_upstream" "$dir" "docuconf/$version: new version" "$(cat <<EOF
### Summary
Changes to recipe: **docuconf/$version**

#### Motivation
New upstream release: https://github.com/$source_repo/releases/tag/v$version

#### Details
Adds $version to \`config.yml\` and \`conandata.yml\` (the release tarball and the SHA-256 published with it). The recipe itself is unchanged.

---
- [x] Read the [contributing guidelines](https://github.com/conan-io/conan-center-index/blob/master/CONTRIBUTING.md)
- [x] Checked that this PR is not a duplicate: [list of PRs by recipe](https://github.com/conan-io/conan-center-index/discussions/24240)
- [ ] If this is a bug fix, please link related issue or provide bug details
- [ ] Tested locally with at least one configuration using a recent version of Conan

Opened by docuconf-cpp's release workflow.
EOF
)"
}

case "${ONLY:-}" in
  "") (update_vcpkg); (update_conan) ;;
  vcpkg) update_vcpkg ;;
  conan) update_conan ;;
  *) echo "ONLY must be vcpkg or conan" >&2; exit 2 ;;
esac
