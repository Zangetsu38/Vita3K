#!/usr/bin/env bash
set -ex

for f in $(find vita3k tools/gen-modules tools/native-tool \( -name *.cpp -o -name *.h \)); do
    if [ "$(diff -u <(cat $f) <(clang-format $f))" != "" ]
    then
        echo "run format"
        exit 1
    fi
done