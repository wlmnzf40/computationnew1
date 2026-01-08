cmake_minimum_required(VERSION 3.14)
project(ComputationGraphAnalysis)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========================================================
# 1. 寻找 LLVM 和 Clang 依赖
# ========================================================
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# 添加 LLVM 模块路径
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

# 应用 LLVM 的预定义宏和包含路径
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

find_package(Clang REQUIRED)
message(STATUS "Found Clang ${CLANG_PACKAGE_VERSION}")
include_directories(${CLANG_INCLUDE_DIRS})

# 【关键修复】自动映射 LLVM 组件到正确的库文件
# 这会根据系统环境自动选择链接 libLLVM.so 还是 libLLVMSupport.a，避免冲突
llvm_map_components_to_libnames(llvm_libs
        support
        core
        option
)

# ========================================================
# 2. 设置头文件路径
# ========================================================
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/include")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/include/code_property_graph")

# ========================================================
# 3. 构建核心库 (ComputationGraphCore)
# ========================================================
file(GLOB CORE_SOURCES "lib/code_property_graph/*.cpp")

add_library(ComputationGraphCore STATIC ${CORE_SOURCES})

# 链接 Clang 库和 映射好的 LLVM 库
target_link_libraries(ComputationGraphCore
        PUBLIC
        clangTooling
        clangFrontend
        clangAST
        clangBasic
        clangSerialization
        clangDriver
        ${llvm_libs}  # <--- 使用这个变量代替手动写 LLVMSupport
)

# 如果是在 Linux 上，可能还需要链接系统库
if(UNIX)
    target_link_libraries(ComputationGraphCore PUBLIC pthread dl z tinfo)
endif()

# ========================================================
# 4. 构建分析工具 (Tools)
# ========================================================

# 工具 1: ComputeGraphTool
add_executable(ComputeGraphTool tools/ComputeGraphTool.cpp)
target_link_libraries(ComputeGraphTool
        PRIVATE
        ComputationGraphCore
)
