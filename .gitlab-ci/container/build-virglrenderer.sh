#!/bin/bash

set -ex

mkdir -p /epoxy
pushd /epoxy
wget -qO- https://github.com/anholt/libepoxy/releases/download/1.5.8/libepoxy-1.5.8.tar.xz | tar -xJ --strip-components=1
meson build/ $EXTRA_MESON_ARGS
ninja -C build install
popd
rm -rf /epoxy

VIRGLRENDERER_VERSION=583961a7ed225a309f9cdad2eeff25c85198acd1
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git --single-branch --no-checkout /virglrenderer
pushd /virglrenderer
git checkout "$VIRGLRENDERER_VERSION"
meson build/ $EXTRA_MESON_ARGS
ninja -C build install
popd
rm -rf /virglrenderer
