#!/bin/sh
# SPDX-License-Identifier: MIT

set -ex

export IGT_FORCE_DRIVER=${DRIVER_NAME}
export PATH=$PATH:/igt/bin/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/igt/lib/aarch64-linux-gnu/:/igt/lib/x86_64-linux-gnu:/igt/lib:/igt/lib64

# Uncomment the below to debug problems with driver probing
: '
ls -l /dev/dri/
cat /sys/kernel/debug/devices_deferred
cat /sys/kernel/debug/device_component/*
'

# Dump drm state to confirm that kernel was able to find a connected display:
set +e
cat /sys/kernel/debug/dri/*/state
set -e

mkdir -p /lib/modules
case "$DRIVER_NAME" in
    amdgpu|vkms)
        # Cannot use HWCI_KERNEL_MODULES as at that point we don't have the module in /lib
        mv /install/modules/lib/modules/* /lib/modules/. || true
        modprobe --first-time $DRIVER_NAME
        ;;
esac

if [ -e "/install/xfails/$DRIVER_NAME-$GPU_VERSION-skips.txt" ]; then
    IGT_SKIPS="--skips /install/xfails/$DRIVER_NAME-$GPU_VERSION-skips.txt"
fi

if [ -e "/install/xfails/$DRIVER_NAME-$GPU_VERSION-flakes.txt" ]; then
    IGT_FLAKES="--flakes /install/xfails/$DRIVER_NAME-$GPU_VERSION-flakes.txt"
fi

if [ -e "/install/xfails/$DRIVER_NAME-$GPU_VERSION-fails.txt" ]; then
    IGT_FAILS="--baseline /install/xfails/$DRIVER_NAME-$GPU_VERSION-fails.txt"
fi

if [ "`uname -m`" = "aarch64" ]; then
    ARCH="arm64"
elif [ "`uname -m`" = "armv7l" ]; then
    ARCH="arm"
else
    ARCH="x86_64"
fi

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 -s $PIPELINE_ARTIFACTS_BASE/$ARCH/igt.tar.gz | tar --zstd -v -x -C /

TESTLIST="/igt/libexec/igt-gpu-tools/ci-testlist.txt"

# If the job is parallel at the gitab job level, take the corresponding fraction
# of the caselist.
if [ -n "$CI_NODE_INDEX" ]; then
    sed -ni $CI_NODE_INDEX~$CI_NODE_TOTAL"p" $TESTLIST
fi

# core_getversion checks if the driver is loaded and probed correctly
# so run it in all shards
if ! grep -q "core_getversion" $TESTLIST; then
    # Add the line to the file
    echo "core_getversion" >> $TESTLIST
fi

set +e
igt-runner \
    run \
    --igt-folder /igt/libexec/igt-gpu-tools \
    --caselist $TESTLIST \
    --output $RESULTS_DIR \
    -vvvv \
    $IGT_SKIPS \
    $IGT_FLAKES \
    $IGT_FAILS \
    --jobs 1
ret=$?
set -e

deqp-runner junit \
   --testsuite IGT \
   --results $RESULTS_DIR/failures.csv \
   --output $RESULTS_DIR/junit.xml \
   --limit 50 \
   --template "See $ARTIFACTS_BASE_URL/results/{{testcase}}.xml"

# Check if /proc/lockdep_stats exists
if [ -f /proc/lockdep_stats ]; then
    # If debug_locks is 0, it indicates lockdep is detected and it turns itself off.
    debug_locks=$(grep 'debug_locks:' /proc/lockdep_stats | awk '{print $2}')
    if [ "$debug_locks" -eq 0 ] && [ "$ret" -eq 0 ]; then
        echo "Warning: LOCKDEP issue detected. Please check dmesg logs for more information."
        cat /proc/lockdep_stats
        ret=101
    fi
fi

cd $oldpath
exit $ret
