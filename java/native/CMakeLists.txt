cmake_minimum_required(VERSION 3.24)
project(ame_jni LANGUAGES NONE)

# Validate the target before trying a platform-specific compiler toolchain.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "AME Java JNI supports only Linux x86_64; target OS: ${CMAKE_SYSTEM_NAME}")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" AME_ARCH)
if(NOT AME_ARCH MATCHES "^(amd64|x86_64|x64)$")
  message(FATAL_ERROR "AME Java JNI supports only Linux x86_64; target CPU: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
enable_language(CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "AME Java JNI requires a 64-bit Linux x86_64 toolchain")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Headless Linux JDKs do not ship AWT; this bridge only needs JNI headers.
find_package(JNI REQUIRED COMPONENTS JVM)
find_package(Threads REQUIRED)
get_filename_component(AME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Build the embedded engine only: no Android, HTTP server, LLM, benchmarks or demos.
foreach(module core storage time vector keyword extractor entity graph_writer seed diffusion rerank engine)
  add_subdirectory("${AME_ROOT}/${module}" "${CMAKE_CURRENT_BINARY_DIR}/modules/${module}")
endforeach()
add_library(ame_jni SHARED ame_jni.cpp)
target_include_directories(ame_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(ame_jni PRIVATE ame_engine Threads::Threads)
# A JNI library is loaded by the JVM; it must not acquire a runtime dependency on libjvm.
set_target_properties(ame_jni PROPERTIES OUTPUT_NAME ame_jni)
set(AME_JAVA_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources" CACHE PATH
    "Resource root for the JAR (contains com/huawei/convai/ame/native); defaults to the build directory")
set(AME_JNI_RESOURCE_PATH "${AME_JAVA_RESOURCE_DIR}/com/huawei/convai/ame/native/linux-x86_64")
add_custom_command(TARGET ame_jni POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${AME_JNI_RESOURCE_PATH}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ame_jni>" "${AME_JNI_RESOURCE_PATH}/$<TARGET_FILE_NAME:ame_jni>"
  VERBATIM)
message(STATUS "AME JNI JAR resource: ${AME_JNI_RESOURCE_PATH}")
