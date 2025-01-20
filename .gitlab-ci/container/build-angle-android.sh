#!/usr/bin/env bash

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# KERNEL_ROOTFS_TAG

set -uex

# Early check for required env variables, relies on `set -u`
: "$ANDROID_NDK_VERSION"
: "$ANDROID_SDK_VERSION"

#uncollapsed_section_start angle "Building angle"

ANDROID_ANGLE_REV="76025caa1a059f464a2b0e8f879dbd4746f092b9"
SCRIPTS_DIR="$(pwd)/.gitlab-ci"
ANGLE_PATCH_DIR="${SCRIPTS_DIR}/container/patches"

# DEPOT tools
git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git /depot-tools
export PATH=/depot-tools:$PATH
export DEPOT_TOOLS_UPDATE=0

[ -d /angle-android ] || mkdir /angle-android

[ -d /angle-android-build ] || mkdir /angle-android-build
echo $SHELL
pushd /angle-android-build

git init
git remote add origin https://chromium.googlesource.com/angle/angle.git
git fetch --depth 1 origin "$ANDROID_ANGLE_REV"
git checkout FETCH_HEAD

angle_patch_files=(
  build-angle_deps_Make-more-sources-conditional.patch
)
for patch in "${angle_patch_files[@]}"; do
  echo "Apply patch to ANGLE from ${patch}"
  GIT_COMMITTER_DATE="$(LC_TIME=C date -d@0)" git am < "${ANGLE_PATCH_DIR}/${patch}"
done

{
  echo "ANGLE base version $ANDROID_ANGLE_REV"
  echo "The following local patches are applied on top:"
  git log --reverse --oneline $ANDROID_ANGLE_REV.. --format='- %s'
} > /angle-android/version

# source preparation
gclient config --name REPLACE-WITH-A-DOT --unmanaged \
  --custom-var='angle_enable_cl=False' \
  --custom-var='angle_enable_cl_testing=False' \
  --custom-var='angle_enable_vulkan_validation_layers=False' \
  --custom-var='angle_enable_wgpu=False' \
  --custom-var='build_allow_regenerate=False' \
  --custom-var='build_angle_deqp_tests=False' \
  --custom-var='build_angle_perftests=False' \
  --custom-var='build_with_catapult=False' \
  --custom-var='build_with_swiftshader=False' \
  --custom-var='checkout_android=True' \
  https://chromium.googlesource.com/angle/angle.git
sed -e 's/REPLACE-WITH-A-DOT/./;' -i .gclient
gclient sync -j"${FDO_CI_CONCURRENT:-4}"


# XXX the following patch seems necessary to avoid some errors when running
# `gn gen out/Android`, investigate and see if there is a better way to handle
# this.
angle_build_patch_files=(
  build-angle-build_deps_fix-build-error.patch
)
pushd build/
for patch in "${angle_build_patch_files[@]}"; do
  echo "Apply patch to ANGLE build from ${patch}"
  GIT_COMMITTER_DATE="$(LC_TIME=C date -d@0)" git am < "${ANGLE_PATCH_DIR}/${patch}"
done
popd


mkdir -p out/Android
cat > out/Android/args.gn <<EOF
angle_build_all=false
angle_build_tests=false
angle_enable_cl=false
angle_enable_cl_testing=false
angle_enable_gl=false
angle_enable_gl_desktop_backend=false
angle_enable_null=false
angle_enable_swiftshader=false
angle_enable_trace=false
angle_enable_wgpu=false
angle_enable_vulkan=true
angle_enable_vulkan_api_dump_layer=false
angle_enable_vulkan_validation_layers=false
angle_has_rapidjson=false
angle_has_frame_capture=false
angle_has_histograms=false
angle_use_custom_libvulkan=false
build_angle_deqp_tests=false
dcheck_always_on=true
enable_expensive_dchecks=false
target_os = "android"
target_cpu = "x64"
is_component_build = false
is_debug = false
angle_assert_always_on = false
android_ndk_version = "${ANDROID_NDK_VERSION}"
android64_ndk_api_level = ${ANDROID_SDK_VERSION}
android32_ndk_api_level = ${ANDROID_SDK_VERSION}
EOF

gn gen out/Android
# depot_tools overrides ninja with a version that doesn't work.  We want
# ninja with FDO_CI_CONCURRENT anyway.
/usr/local/bin/ninja -C out/Android/ libEGL libGLESv2

rm -f out/Android/libvulkan.so* out/Android/*.so.TOC
cp out/Android/lib*.so* /angle-android/

rm -rf out

popd
rm -rf /depot-tools
rm -rf /angle-android-build

find /angle-android

#section_end angle
