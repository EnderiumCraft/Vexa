#!/bin/sh
# Runs all of Servo's and Vane's tests in the Vexa VM, one after the other, and says which passed.
#
#   run-vane-tests.sh WORKDIR
#
# WORKDIR is the one tools/servo/build-servo-vexa.sh built (`make vane` makes build/vane-work),
# with servo-render and vane in it; build/vexa.iso must have been built (with Vane, for the last
# test: `make vane iso`). Each test is its own script here, and has its own notes.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=${1:?usage: run-vane-tests.sh WORKDIR}
failed=""
for test in run-render-test run-net-test run-https-test run-window-test run-address-test \
        run-tabs-test run-context-test; do
    if python3 "$here/$test.py" "$work" > "$work/$test.log" 2>&1; then
        echo "PASS $test"
    else
        echo "FAIL $test (see $work/$test.log)"
        failed="$failed $test"
    fi
done
if [ -f "$here/../../build/vane/vane" ]; then
    if python3 "$here/run-vane-iso-test.py" > "$work/run-vane-iso-test.log" 2>&1; then
        echo "PASS run-vane-iso-test"
    else
        echo "FAIL run-vane-iso-test (see $work/run-vane-iso-test.log)"
        failed="$failed run-vane-iso-test"
    fi
fi
[ -z "$failed" ]
