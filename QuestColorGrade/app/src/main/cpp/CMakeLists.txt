cmake_minimum_required(VERSION 3.22.1)
project(questcolor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Загрузчик OpenXR приходит из AAR (org.khronos.openxr:openxr_loader_for_android) через Prefab.
find_package(OpenXR REQUIRED CONFIG)

# android_native_app_glue лежит в NDK
if(NOT DEFINED ANDROID_NDK)
  set(ANDROID_NDK ${CMAKE_ANDROID_NDK})
endif()
set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(questcolor SHARED
  main.cpp
  ${GLUE_DIR}/android_native_app_glue.c)

target_include_directories(questcolor PRIVATE ${GLUE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(questcolor PRIVATE -Wall)
target_link_libraries(questcolor PRIVATE OpenXR::openxr_loader android log EGL GLESv3)
# Выравнивание под 16 КБ страницы (требование новых версий Android)
target_link_options(questcolor PRIVATE -Wl,-z,max-page-size=16384)
