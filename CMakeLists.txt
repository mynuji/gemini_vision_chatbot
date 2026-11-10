cmake_minimum_required(VERSION 3.16)
add_compile_definitions(MALLOC_CAP_SIMD=0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gemini_vision_chatbot)
