#!/bin/bash

set -ex

if [ -n "$INCLUDE_OPENCL_TESTS" ]; then
    PIGLIT_OPTS="-DPIGLIT_BUILD_CL_TESTS=ON"
fi

git clone https://gitlab.freedesktop.org/mesa/piglit.git --single-branch --no-checkout /piglit
pushd /piglit
git checkout 4ac05e80170205f56a9c66d5ed5a0c73eb580bab
patch -p1 <$OLDPWD/.gitlab-ci/piglit/disable-vs_in.diff
cmake -S . -B . -G Ninja -DCMAKE_BUILD_TYPE=Release $PIGLIT_OPTS $EXTRA_CMAKE_ARGS
ninja $PIGLIT_BUILD_TARGETS
find -name .git -o -name '*ninja*' -o -iname '*cmake*' -o -name '*.[chao]' | xargs rm -rf
rm -rf target_api
if [ "x$PIGLIT_BUILD_TARGETS" = "xpiglit_replayer" ]; then
    find ! -regex "^\.$" \
         ! -regex "^\.\/piglit.*" \
         ! -regex "^\.\/framework.*" \
         ! -regex "^\.\/bin$" \
         ! -regex "^\.\/bin\/replayer\.py" \
         ! -regex "^\.\/templates.*" \
         ! -regex "^\.\/tests$" \
         ! -regex "^\.\/tests\/replay\.py" 2>/dev/null | xargs rm -rf
fi
popd
