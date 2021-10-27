#!/bin/sh

set -e

export DEQP_TEMP_DIR=$1

export EGL_LOG_LEVEL=debug

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev || echo possibly already mounted
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp

. $DEQP_TEMP_DIR/crosvm-env.sh

cd $PWD

set +e
if sh $DEQP_TEMP_DIR/crosvm-script.sh; then
    touch $CI_PROJECT_DIR/results/success
fi
set -e

sleep 5   # Leave some time to get the last output flushed out

poweroff -d -n -f || true

sleep 10   # Just in case init would exit before the kernel shuts down the VM

exit 1