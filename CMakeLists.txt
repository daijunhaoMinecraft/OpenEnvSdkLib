cmake_minimum_required(VERSION 4.1)
project(envsdklib)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================================
# 1. 查找第三方库
# ==========================================

# 查找 OpenSSL (cpp-httplib 发起 HTTPS 请求的必需前置)
find_package(OpenSSL REQUIRED)

# 查找 nlohmann/json (JSON 解析)
find_package(nlohmann_json CONFIG REQUIRED)

# 查找 cpp-httplib (HTTP 客户端)
find_package(httplib CONFIG REQUIRED)

# ==========================================
# 2. 定义编译目标 (生成 EnvSDKLib.dll)
# ==========================================
add_library(envsdklib SHARED library.cpp)

# ==========================================
# 3. 链接第三方库与宏定义
# ==========================================

# 让 CMake 全局定义宏（推荐用法，确保 httplib 正确启用 SSL）
target_compile_definitions(envsdklib PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# 链接所有查找到的依赖库
target_link_libraries(envsdklib PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        httplib::httplib
)

# ==========================================
# 4. Windows 平台特有系统网络库
# ==========================================
if(WIN32)
    # Windows 下编译网络库与加密库通常需要附带链接系统的 winsock 和 crypt 库
    target_link_libraries(envsdklib PRIVATE ws2_32 crypt32)
endif()

find_package(pcre2 CONFIG REQUIRED)
target_link_libraries(envsdklib PRIVATE PCRE2::8BIT PCRE2::16BIT PCRE2::32BIT PCRE2::POSIX)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(envsdklib PRIVATE minhook::minhook)