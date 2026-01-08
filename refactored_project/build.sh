#!/bin/bash
# 自动编译脚本

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

echo "==================================="
echo "ComputeGraph 重构项目编译脚本"
echo "==================================="
echo ""

# 检查依赖
echo "检查依赖..."
if ! command -v cmake &> /dev/null; then
    echo "❌ 错误: 未找到cmake"
    echo "请安装: sudo apt-get install cmake"
    exit 1
fi
echo "✓ cmake已安装"

if ! command -v clang++ &> /dev/null; then
    echo "❌ 错误: 未找到clang++"
    echo "请安装: sudo apt-get install clang"
    exit 1
fi
echo "✓ clang++已安装"

# 清理旧构建
if [ -d "$BUILD_DIR" ]; then
    echo ""
    echo "清理旧构建目录..."
    rm -rf "$BUILD_DIR"
fi

# 创建构建目录
echo "创建构建目录..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 运行CMake
echo ""
echo "运行CMake配置..."
cmake .. || {
    echo "❌ CMake配置失败"
    exit 1
}

# 编译
echo ""
echo "开始编译..."
make -j$(nproc) || {
    echo "❌ 编译失败"
    exit 1
}

echo ""
echo "==================================="
echo "✅ 编译成功!"
echo "==================================="
echo ""
echo "可执行文件: $BUILD_DIR/compute_graph_tool"
echo ""
echo "运行测试:"
echo "  ./compute_graph_tool ../case1"
echo "  ./compute_graph_tool ../case2"
echo ""
