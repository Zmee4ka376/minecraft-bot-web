cmake_minimum_required(VERSION 3.19)

project(WebBot)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

set(BOTCRAFT_GAME_VERSION "1.16.5" CACHE STRING "Game version")
set(BOTCRAFT_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Output dir")
set(BOTCRAFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOTCRAFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOTCRAFT_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BOTCRAFT_USE_OPENGL_GUI OFF CACHE BOOL "" FORCE)
set(BOTCRAFT_COMPRESSION ON CACHE BOOL "" FORCE)
set(BOTCRAFT_ENCRYPTION ON CACHE BOOL "" FORCE)

add_subdirectory(Botcraft)

add_executable(WebBot
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/WebBotClient.cpp
    ${CMAKE_SOURCE_DIR}/include/WebBotClient.hpp
)
target_include_directories(WebBot PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/Botcraft/3rdparty/asio/asio/include
)
target_compile_definitions(WebBot PUBLIC ASIO_STANDALONE _WIN32_WINNT=0x0501)
target_link_libraries(WebBot botcraft ws2_32)
set_target_properties(WebBot PROPERTIES CXX_STANDARD 17)

if(MSVC)
    set_target_properties(WebBot PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_target_properties(WebBot PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BOTCRAFT_OUTPUT_DIR}/bin")
    set_target_properties(WebBot PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BOTCRAFT_OUTPUT_DIR}/bin")
    set_target_properties(WebBot PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${BOTCRAFT_OUTPUT_DIR}/bin")
    set_target_properties(WebBot PROPERTIES DEBUG_POSTFIX "_d")
    set_target_properties(WebBot PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${BOTCRAFT_OUTPUT_DIR}/bin")
else()
    set_target_properties(WebBot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BOTCRAFT_OUTPUT_DIR}/bin")
endif()
