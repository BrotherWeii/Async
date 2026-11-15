cmake_minimum_required(VERSION 3.15)
project(ThreadPool CXX)

# 库的地板是 C++11, 与 ThreadPool.h 保持一致(不用 C++14/17)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC 下中文注释/输出需要 /utf-8 + /EHsc; 其余平台开全警告
if(MSVC)
    add_compile_options(/utf-8 /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()

# 让 FOLDER 属性生效: VS 解决方案里按文件夹分组显示工程
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── 线程池库(header-only, INTERFACE 目标只提供头文件路径与线程依赖) ──
# 头文件一并列出, 否则 VS Solution Explorer 不显示 (CMake 只展示显式列出的文件)
add_library(ThreadPool INTERFACE
    src/ThreadPool.h
)
target_include_directories(ThreadPool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 标准线程库依赖(GCC/Clang 拿到 -pthread, MSVC 无副作用)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(ThreadPool INTERFACE Threads::Threads)

# ── 自测用例 (可关: -DBUILD_TEST=OFF 只用库) ──
option(BUILD_TEST "构建自测用例与学习示例" ON)
if(BUILD_TEST)
    add_subdirectory(test)
endif()
