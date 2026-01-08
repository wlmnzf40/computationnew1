# 快速开始指南

## 1分钟快速编译

```bash
cd refactored_project
./build.sh
```

就这么简单！

## 运行测试

```bash
cd build
./compute_graph_tool ../case1
./compute_graph_tool ../case2
```

## 查看输出

生成的DOT文件在：
- `case1_graph.dot`
- `case2_graph.dot`

可视化：
```bash
dot -Tpng case1_graph.dot -o case1_graph.png
dot -Tpng case2_graph.dot -o case2_graph.png
```

## 如果遇到问题

### 1. CMake找不到LLVM

```bash
# 设置LLVM路径
export LLVM_DIR=/usr/lib/llvm-12/lib/cmake/llvm
export Clang_DIR=/usr/lib/llvm-12/lib/cmake/clang
```

### 2. 缺少依赖

```bash
# Ubuntu/Debian
sudo apt-get install cmake clang-12 llvm-12-dev libclang-12-dev

# 或者使用更新的版本
sudo apt-get install cmake clang-14 llvm-14-dev libclang-14-dev
```

### 3. 编译错误

检查：
- CMake版本 ≥ 3.13
- Clang版本 ≥ 12
- C++标准 = C++17

### 4. 运行时错误

确保测试文件存在：
```bash
ls case1 case2
```

## 项目结构

```
refactored_project/
├── build.sh              ← 运行这个
├── CMakeLists.txt
├── case1, case2          ← 测试用例
├── include/              ← 头文件
└── lib/                  ← 源文件
```

## 重构详情

查看详细报告：
- `README.md` - 项目概述
- `REFACTORING_REPORT.md` - 详细重构报告

## 技术支持

如有问题，请检查：
1. README.md - 基本信息
2. REFACTORING_REPORT.md - 技术细节
3. 编译输出 - 错误信息

---
**快速开始指南** | 2026-01-07
