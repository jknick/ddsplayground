"""Repository rule: downloads CycloneDDS source and builds it with cmake."""

_CYCLONEDDS_VERSION = "0.10.5"
_CYCLONEDDS_URL = "https://github.com/eclipse-cyclonedds/cyclonedds/archive/refs/tags/{v}.tar.gz".format(
    v = _CYCLONEDDS_VERSION,
)
_CYCLONEDDS_STRIP_PREFIX = "cyclonedds-{v}".format(v = _CYCLONEDDS_VERSION)

# sha256 is intentionally omitted so Bazel prints it on first fetch.
# After the first build, paste the printed sha256 here for reproducibility.

def _cpu_count(rctx):
    """Return number of logical CPUs as a string."""
    r = rctx.execute(["sysctl", "-n", "hw.logicalcpu"])
    if r.return_code == 0:
        return r.stdout.strip()
    r = rctx.execute(["nproc"])
    if r.return_code == 0:
        return r.stdout.strip()
    return "4"

def _cyclonedds_repository_impl(rctx):
    # ── 1. Download source ────────────────────────────────────────────────────
    rctx.download_and_extract(
        url = _CYCLONEDDS_URL,
        stripPrefix = _CYCLONEDDS_STRIP_PREFIX,
        # sha256 = "TODO: fill in after first successful build",
    )

    cmake = rctx.which("cmake")
    if cmake == None:
        fail("cmake not found in PATH. Install cmake (e.g. brew install cmake).")

    # ── 2. Configure ─────────────────────────────────────────────────────────
    res = rctx.execute(
        [
            cmake, "-S", ".", "-B", "_build",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=_install",
            "-DBUILD_SHARED_LIBS=OFF",   # static ddsc → self-contained idlc
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_TESTING=OFF",
            "-DENABLE_SSL=OFF",
            "-DENABLE_SECURITY=OFF",
        ],
        timeout = 300,
        quiet = False,
    )
    if res.return_code != 0:
        fail("CycloneDDS cmake configure failed:\nSTDOUT:\n{}\nSTDERR:\n{}".format(
            res.stdout, res.stderr))

    # ── 3. Build ──────────────────────────────────────────────────────────────
    res = rctx.execute(
        [cmake, "--build", "_build", "--config", "Release", "--parallel", _cpu_count(rctx)],
        timeout = 1800,
        quiet = False,
    )
    if res.return_code != 0:
        fail("CycloneDDS cmake build failed:\nSTDOUT:\n{}\nSTDERR:\n{}".format(
            res.stdout, res.stderr))

    # ── 4. Install ────────────────────────────────────────────────────────────
    res = rctx.execute(
        [cmake, "--install", "_build", "--config", "Release"],
        timeout = 120,
        quiet = False,
    )
    if res.return_code != 0:
        fail("CycloneDDS cmake install failed:\nSTDOUT:\n{}\nSTDERR:\n{}".format(
            res.stdout, res.stderr))

    # ── 5. Create wrapper script so idlc is usable as a Bazel sh_binary tool ─
    rctx.file("idlc_run.sh", content = """\
#!/usr/bin/env bash
# Wrapper that forwards all args to the CycloneDDS IDL compiler.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/_install/bin/idlc" "$@"
""", executable = True)

    # ── 6. Emit BUILD file ────────────────────────────────────────────────────
    rctx.file("BUILD.bazel", content = """\
load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

package(default_visibility = ["//visibility:public"])

# Pre-built static CycloneDDS runtime library
cc_import(
    name = "ddsc_prebuilt",
    static_library = "_install/lib/libddsc.a",
)

cc_library(
    name = "ddsc",
    hdrs = glob(["_install/include/**/*.h"]),
    strip_include_prefix = "_install/include",
    deps = [":ddsc_prebuilt"],
    linkopts = ["-lpthread"],
)

# idlc IDL compiler – expose as a single-file filegroup so it can be used
# directly in genrule tools / $(execpath ...).
filegroup(
    name = "idlc",
    srcs = ["_install/bin/idlc"],
)
""")

cyclonedds_repository = repository_rule(
    implementation = _cyclonedds_repository_impl,
    attrs = {},
    doc = "Downloads and builds CycloneDDS from source using cmake.",
)
