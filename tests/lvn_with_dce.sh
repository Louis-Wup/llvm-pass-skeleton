#!/bin/bash

mkdir -p build
cd build && cmake .. && make LVNPass DCEPass && cd ..

mkdir -p output/LVN_with_DCE
echo "--- Running LVN+DCE tests ---"

for f in tests/LVN_with_DCE/*.cpp; do
    name=$(basename "$f" .cpp)
    clang -emit-llvm "$f" -S -o "output/LVN_with_DCE/$name.ll"
    clang -fpass-plugin=`echo ./build/skeleton/LVNPass.so` -fpass-plugin=`echo ./build/skeleton/DCEPass.so` -emit-llvm "$f" -S -o "output/LVN_with_DCE/$name.opt.ll"

    clang "output/LVN_with_DCE/$name.ll" -o "output/LVN_with_DCE/$name.out"
    clang "output/LVN_with_DCE/$name.opt.ll" -o "output/LVN_with_DCE/$name.opt.out"

    ./output/LVN_with_DCE/$name.out
    expected=$?
    ./output/LVN_with_DCE/$name.opt.out
    actual=$?

    if [ $expected -eq $actual ]; then
        echo "PASS: $name (Exit Code: $expected)"
    else
        echo "FAIL: $name (Expected: $expected, Actual: $actual)"
        exit 1
    fi
done

echo "--- LVN+DCE tests completed ---"
