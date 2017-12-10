#! /bin/bash

dir="$1"
num="$2"
test -z "$dir" -o -z "$num" && exit 1

touch "$dir"/running$num
if test -z "$ICERUN_TEST_VALGRIND"; then
    sleep 0.2
else
    sleep 1
fi
rm "$dir"/running$num
touch "$dir"/done$num
exit 0
