#!/bin/bash
BOOST_INC=$(brew --prefix boost)/include
ABSL_FLAGS=$(pkg-config --cflags --libs absl_btree)
g++ -std=c++23 -I"$BOOST_INC" $ABSL_FLAGS "$@" -o tests && ./tests
