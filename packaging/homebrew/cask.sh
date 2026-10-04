#!/usr/bin/env bash
# Prints the Homebrew cask for one channel, for homebrew.yml to write into
# the tap. Everything that changes per build comes in as an argument, so the
# same text can be produced by hand for a given release.
#
#   cask.sh release <owner/repo> <version> <sha256 arm> <sha256 intel>
#   cask.sh nightly <owner/repo> <version> <sha256 arm> <sha256 intel>
#
# <version> is the archive's own: 1.2.3 for a release, 0.1.0+<sha8> for a
# nightly. It names the directory inside the tarball, which is why the cask
# interpolates it into the app path rather than only the URL.
set -euo pipefail

channel=$1 repo=$2 version=$3 sha_arm=$4 sha_intel=$5

case "$channel" in
  release)
    token=sweeppp
    name='Sweep++'
    url="https://github.com/$repo/releases/download/v#{version}/sweeppp-#{version}-macos-#{arch}.tar.gz"
    suffix=''
    ;;
  nightly)
    token=sweeppp-nightly
    name='Sweep++ Nightly'
    # The asset keeps one name across builds, and Homebrew caches downloads
    # by URL: without the query the next nightly would be checked against
    # the previous one's cached tarball and refused. GitHub ignores it.
    url="https://github.com/$repo/releases/download/nightly/sweeppp-nightly-macos-#{arch}.tar.gz?build=${version##*+}"
    suffix='-nightly'
    ;;
  *)
    echo "cask.sh: channel must be release or nightly, not '$channel'" >&2
    exit 2
    ;;
esac

# The nightly's CLI tools are symlinked under its own names so it installs
# beside a release, as the .deb does; the release keeps the names in the
# archive.
target() {
  [[ -z "$suffix" ]] || printf ', target: "%s"' "$1"
}

# Unquoted so $repo and friends expand; Ruby's #{...} is not shell syntax and
# passes through.
cat <<EOF
cask "$token" do
  arch arm: "arm", intel: "intel"

  version "$version"
  sha256 arm:   "$sha_arm",
         intel: "$sha_intel"

  url "$url"
  name "$name"
  desc "Wideband spectrum analyser for software-defined radios"
  homepage "https://sweeppp.app/"

  depends_on formula: "glfw"
  depends_on macos: ">= :ventura"

  app "sweeppp-#{version}-macos-#{arch}/$name.app"
  binary "sweeppp-#{version}-macos-#{arch}/sweeppp-cli"$(target "sweeppp$suffix-cli")
  binary "sweeppp-#{version}-macos-#{arch}/sweeps"$(target "sweeps$suffix")

  zap trash: "~/Library/Application Support/sweeppp$suffix"

  caveats <<~CAVEATS
    The build is not signed. If macOS refuses to open it, run:
      xattr -dr com.apple.quarantine "/Applications/$name.app"
  CAVEATS
end
EOF
