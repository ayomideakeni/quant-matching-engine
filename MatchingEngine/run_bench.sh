#!/bin/bash
BOOST_INC=$(brew --prefix boost)/include
ABSL_FLAGS=$(pkg-config --cflags --libs absl_btree)
g++ -O3 -std=c++23 -I"$BOOST_INC" $ABSL_FLAGS "$@" -o benchmark && ./benchmark
