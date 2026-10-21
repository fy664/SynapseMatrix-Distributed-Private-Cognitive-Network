# SynapseMatrix — C++ Runtime 工程（Month 3）
#
# 职责：构建规格书 Month 3 的 C++ 多线程 Task Runtime
#   （Device / Worker / Task / Queue / Executor / Telemetry）
#
# 环境：Windows + MSVC（vcvars64）+ CMake；C++20
# 用法：
#   cmd /c ""D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -S runtime -B runtime/build && cmake --build runtime/build --config Release"
#   或直接 cmake -S runtime -B build（若 cl 已在 PATH）

cmake_minimum_required(VERSION 3.20)
project(synapse_runtime LANGUAGES CXX)

# ---- 全局编译标准：C++20 ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- MSVC 专属：UTF-8 源码 + 无异常展开限制 + 全警告 ----
if(MSVC)
    add_compile_options(/utf-8)     # 中文注释必须：否则 C4819 报错
    add_compile_options(/EHsc)      # C++ 异常处理（thread 内抛异常不崩进程）
    add_compile_options(/W4)        # 四级警告全开（比默认 /W3 更严）
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- 库：runtime 核心（当前仅 Queue/Task，Worker/Executor 由用户实现后并入）----
add_library(synapse_core STATIC
    core/src/queue.cpp
)

target_include_directories(synapse_core PUBLIC core/include)

# ---- 测试与演示 ----
enable_testing()

# 最小线程池示例（教学 + 工具链验证，编译运行即证明 thread/mutex/CV/atomic 可用）
add_executable(demo_threadpool core/tests/demo_threadpool.cpp)
target_link_libraries(demo_threadpool PRIVATE synapse_core)

# 单测：线程安全队列
add_executable(test_queue core/tests/test_queue.cpp)
target_link_libraries(test_queue PRIVATE synapse_core)
add_test(NAME test_queue COMMAND test_queue)

# 单测：Task 基础语义
add_executable(test_task core/tests/test_task.cpp)
target_link_libraries(test_task PRIVATE synapse_core)
add_test(NAME test_task COMMAND test_task)
