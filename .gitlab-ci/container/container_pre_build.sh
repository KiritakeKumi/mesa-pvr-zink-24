#!/bin/sh

if test -x /usr/bin/ccache; then
    if test -f /etc/debian_version; then
        CCACHE_PATH=/usr/lib/ccache
    elif test -f /etc/alpine-release; then
        CCACHE_PATH=/usr/lib/ccache/bin
    else
        CCACHE_PATH=/usr/lib64/ccache
    fi

    # Common setup among container builds before we get to building code.

    export CCACHE_COMPILERCHECK=content
    export CCACHE_COMPRESS=true
    export CCACHE_DIR="/cache/$CI_PROJECT_NAME/ccache"
    export PATH="$CCACHE_PATH:$PATH"

    # CMake ignores $PATH, so we have to force CC/GCC to the ccache versions.
    export CC="${CCACHE_PATH}/gcc"
    export CXX="${CCACHE_PATH}/g++"

    ccache --show-stats
fi

# Make a wrapper script for ninja to always include the -j flags
{
    echo '#!/bin/sh -x'
    # shellcheck disable=SC2016
    echo '/usr/bin/ninja -j${FDO_CI_CONCURRENT:-4} "$@"'
} > /usr/local/bin/ninja
chmod +x /usr/local/bin/ninja

# Set MAKEFLAGS so that all make invocations in container builds include the
# flags (doesn't apply to non-container builds, but we don't run make there)
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# make wget to try more than once, when download fails or timeout
echo -e "retry_connrefused = on\n" \
        "read_timeout = 300\n" \
        "tries = 4\n" \
	"retry_on_host_error = on\n" \
	"retry_on_http_error = 429,500,502,503,504\n" \
        "wait_retry = 32" >> /etc/wgetrc

ci_tag_early_checks() {
    # Runs the first part of the build script to perform the tag check only
    uncollapsed_section_switch "ci_tag_early_checks" "Running Structured Tagging early checks"
    echo "[Structured Tagging] Checking components: ${CI_BUILD_COMPONENTS}"
    local bin_ci_dir
    bin_ci_dir="$(git rev-parse --show-toplevel)"/bin/ci
    # shellcheck disable=SC2086
    "${bin_ci_dir}"/update_tag.sh --check ${CI_BUILD_COMPONENTS} || exit 1
    echo "[Structured Tagging] Components check done"
    section_end "ci_tag_early_checks"
}

# Check if each declared tag component is up to date before building
if [ -n "${CI_BUILD_COMPONENTS:-}" ]; then
    # Remove leading ${CI_BUILD_COMPONENTS%...} from the prefix
    CI_BUILD_COMPONENTS="${CI_BUILD_COMPONENTS//\$\{CI_BUILD_COMPONENTS%*\} /}"
    ci_tag_early_checks
fi
