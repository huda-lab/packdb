#!/usr/bin/env bash
clear
set -o pipefail

cd build/release

if make -j$(nproc); then
  ./packdb ../../packdb.db < ../../test/packdb/test.sql
else
  echo "Build failed." >&2
  exit 1
fi
