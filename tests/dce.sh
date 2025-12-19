#!/bin/bash

mkdir -p build
cd build && cmake .. && make DCEPass && cd ..

mkdir -p output/DCE
echo "--- Running DCE tests ---"

for f in tests/DCE/*.cpp; do
    name=$(basename "$f" .cpp)
    clang -emit-llvm "$f" -S -o "output/DCE/$name.ll"
    clang -fpass-plugin=`echo ./build/skeleton/DCEPass.so` -emit-llvm "$f" -S -o "output/DCE/$name.opt.ll"

    clang "output/DCE/$name.ll" -o "output/DCE/$name.out"
    clang "output/DCE/$name.opt.ll" -o "output/DCE/$name.opt.out"

    ./output/DCE/$name.out
    expected=$?
    ./output/DCE/$name.opt.out
    actual=$?

    if [ $expected -eq $actual ]; then
        echo "PASS: $name (Exit Code: $expected)"
    else
        echo "FAIL: $name (Expected: $expected, Actual: $actual)"
        exit 1
    fi
done

echo "--- DCE tests completed ---"
