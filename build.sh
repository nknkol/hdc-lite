#!/bin/bash

set -e

echo "========================================="
echo "HDC Build Script - Original Version"
echo "========================================="

# 构建Docker镜像
echo ""
echo "Step 1: Building Docker image..."
docker build -t hdc-dev:latest .

# 在容器中编译
echo ""
echo "Step 2: Compiling in container..."
docker run --rm -v $(pwd):/workspace hdc-dev:latest bash -c "
    cd /workspace && \
    make -f Makefile.simple clean && \
    make -f Makefile.simple all && \
    make -f Makefile.simple test
"

echo ""
echo "========================================="
echo "✓ Build completed!"
echo "========================================="
echo ""
echo "Executable: ./build/hdc"
echo ""
echo "Next steps:"
echo "  1. Test run: ./run.sh shell"
echo "  2. Start server: ./build/hdc start"
echo "  3. Check version: ./build/hdc -v"