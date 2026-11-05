cmake_minimum_required(VERSION 3.16)
project(wow64_mpq_proxy CXX)

if(NOT MSVC)
    message(FATAL_ERROR "请用 MSVC 构建（GitHub Actions 的 windows-latest 自带，无需本地装环境）")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 1. 代理 DLL：产物 DINPUT8.dll ----
add_library(DINPUT8 SHARED
    src/dinput8_proxy.cpp
    src/dinput8.def
)
set_target_properties(DINPUT8 PROPERTIES
    OUTPUT_NAME "DINPUT8"
    PREFIX ""
    SUFFIX ".dll"
)
target_compile_options(DINPUT8 PRIVATE /MT /O2 /EHsc /GS- /utf-8)
target_link_options(DINPUT8 PRIVATE /DYNAMICBASE /NXCOMPAT)

# ---- 2. 算法自检：验证 512 字节还原公式 ----
add_executable(selftest src/dinput8_proxy.cpp)
target_compile_definitions(selftest PRIVATE SELFTEST)
target_compile_options(selftest PRIVATE /MT /O2 /EHsc /utf-8)

# ---- 3. 端到端测试：实际加载 DINPUT8.dll 再读一次加密文件 ----
add_executable(hooktest tools/hooktest.cpp)
target_compile_options(hooktest PRIVATE /MT /O2 /EHsc /utf-8)
add_dependencies(hooktest DINPUT8)
