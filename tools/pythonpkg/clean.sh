#!/bin/sh

SCRIPT=$(realpath "$0")
SCRIPTPATH=$(dirname "$SCRIPT")

echo $SCRIPTPATH

rm -rf	$SCRIPTPATH/.eggs \
		$SCRIPTPATH/.pytest_cache \
		$SCRIPTPATH/build \
		$SCRIPTPATH/duckdb_build \
		$SCRIPTPATH/dist \
		$SCRIPTPATH/packdb.egg-info \
		$SCRIPTPATH/packdb.cpp \
		$SCRIPTPATH/packdb.hpp \
		$SCRIPTPATH/parquet_extension.cpp \
		$SCRIPTPATH/parquet_extension.hpp \
		$SCRIPTPATH/duckdb_tarball

rm -f	$SCRIPTPATH/sources.list \
		$SCRIPTPATH/includes.list \
		$SCRIPTPATH/githash.list

python3 -m pip uninstall packdb --yes 2>/dev/null || true
