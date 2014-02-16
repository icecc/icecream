#! /bin/bash

dir="$1"
num="$2"
test -z "$dir" -o -z "$num" && exit 1

touch "$dir"/running$num
sleep 0.2
rm "$dir"/running$num
touch "$dir"/done$num
exit 0
