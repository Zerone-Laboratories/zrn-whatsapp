# The phone's cross toolchain of zrn-sdk, plus C++ (the SDK's file sets only the C compiler).
set(_sdk "${CMAKE_CURRENT_LIST_DIR}/../../zrn-sdk")
if(DEFINED ENV{ZRN_SDK_DIR})
	set(_sdk "$ENV{ZRN_SDK_DIR}")
endif()
include("${_sdk}/cmake/toolchain-aarch64.cmake")
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)
