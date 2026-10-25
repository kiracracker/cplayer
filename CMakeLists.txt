cmake_minimum_required(VERSION 3.30)
project(mp3player CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

add_executable(mp3player WIN32
    src/main.cpp
    src/cover.cpp
    src/ui.cpp
    #  src/player.cpp
    src/utils.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/favicon.rc
)

target_include_directories(mp3player PRIVATE
    include
    include/raylib
    include/taglib
    include/taglib/mpeg
    include/taglib/toolkit
    
    include/harfbuzz
    ${CURL_INCLUDE_DIRS}
)

target_link_directories(mp3player PRIVATE
   
    ${CURL_STATIC_LIBRARY_DIRS}
)

target_link_libraries(mp3player PRIVATE
    ${PROJECT_SOURCE_DIR}/lib/libtag.a
    ${PROJECT_SOURCE_DIR}/lib/libraylib.a
    C:/msys64/mingw64/lib/libSDL2_ttf.a
  ${PROJECT_SOURCE_DIR}/lib/libharfbuzz.a
  ${PROJECT_SOURCE_DIR}/lib/libfreetype.a
    z
    winmm
    gdi32
    opengl32
    ole32
    shell32
    user32
    comdlg32
    ${CURL_STATIC_LIBRARIES}
    ws2_32
    crypt32
    freetype 
    png 
    bz2
)

target_compile_options(mp3player PRIVATE
    
    ${CURL_STATIC_CFLAGS_OTHER}
)


target_link_options(mp3player PRIVATE -static-libgcc -static-libstdc++ -static)

