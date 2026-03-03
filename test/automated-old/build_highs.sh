#!/bin/bash
# Build HiGHS as a static executable

set -e

cd "$(dirname "$0")"

echo "Cleaning up old builds..."
rm -rf highs_build highs

echo "Cloning HiGHS..."
git clone --depth 1 https://github.com/ERGO-Code/HiGHS.git highs_build

echo "Configuring HiGHS with static linking..."
cd highs_build
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"

echo "Building HiGHS..."
make -j$(nproc)

echo "Copying executable..."
cp bin/highs ../../highs
chmod +x ../../highs

echo "Cleaning up build directory..."
cd ../..
rm -rf highs_build

echo "Testing HiGHS..."
./highs --version

echo ""
echo "✓ HiGHS built successfully!"
echo "  Location: $(pwd)/highs"
