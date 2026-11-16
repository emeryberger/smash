# Homebrew formula for smash (compression-aware malloc).
#
# Current Homebrew refuses to install a formula from a bare file path; put it
# in a tap (e.g. a emeryberger/homebrew-smash repo, or locally:
#   brew tap-new --no-git local/smash && cp smash.rb "$(brew --repo local/smash)/Formula/"
#   brew install --build-from-source local/smash/smash && brew test local/smash/smash
# ). See agents/releases.md ("Homebrew").
#
# Builds from the source tarball of a tagged GitHub release. The install tree
# is the layout contract from CMakeLists.txt ("Install"):
#   #{prefix}/lib/libsmash.dylib   (fat arm64 + arm64e on Apple Silicon, so it
#                                   can interpose into arm64e processes too)
#   #{prefix}/bin/smash            launcher
#   #{prefix}/include/smash/       public headers
#
# The first configure fetches alloc8, printf, LZ4 and zstd with CMake
# FetchContent (same as every other smash build), so the build needs network
# access; that is fine for a tap but would need `resource` blocks before a
# homebrew-core submission.
class Smash < Formula
  desc "Compression-aware malloc that transparently compresses cold heap pages"
  homepage "https://github.com/emeryberger/smash"
  # PLACEHOLDERS: url must name a release tag that contains the CMake install()
  # rules (v0.0.3-alpha and earlier predate them). sha256 is NOT a real hash:
  # release automation / `brew bump-formula-pr --url=<tarball>` fills in the
  # sha256 of the published tarball. Do not hand-edit a guessed value in.
  url "https://github.com/emeryberger/smash/archive/refs/tags/v0.0.4-alpha.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "Apache-2.0"
  head "https://github.com/emeryberger/smash.git", branch: "master"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DSMASH_BUILD_TESTS=OFF",
           "-DSMASH_BUILD_BENCH=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--target", "smash"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Run a program under smash with:
        smash ./your_program
      macOS strips DYLD_* variables for SIP-protected system binaries
      (/bin, /usr/bin), so smash applies to your own programs.
      Small, short-lived programs show no effect: pages are compressed only
      after they go untouched for 10 s (SMASH_COLD_TIMEOUT_SEC).
    EOS
  end

  test do
    (testpath/"probe.c").write <<~C
      #include <stdio.h>
      #include <stdlib.h>
      #include <string.h>
      int main(void) {
        char *p = malloc(4 << 20);
        if (!p) return 1;
        memset(p, 'a', 4 << 20);
        free(p);
        puts("probe ok");
        return 0;
      }
    C
    system ENV.cc, "probe.c", "-o", "probe"
    assert_path_exists lib/shared_library("libsmash")
    output = shell_output("SMASH_STATS=1 #{bin}/smash ./probe 2>&1")
    assert_match "probe ok", output
    assert_match "[smash stats]", output
  end
end
