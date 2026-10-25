cplayer

A native desktop music player built with C++17.

A personal music player project exploring native Windows development, multimedia libraries, audio metadata, and text rendering.

<p align="center">
  <img src="screenshots/main-window.png" alt="cplayer main window" width="800">
</p>
<p align="center">
  <em>A work in progress.</em>
</p>
<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus" alt="C++17">
  <img src="https://img.shields.io/badge/Platform-Windows-0078D4?logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/Build-CMake-064F8C?logo=cmake" alt="CMake">
  <img src="https://img.shields.io/badge/Compiler-MinGW--w64-555555" alt="MinGW-w64">
</p>

Overview

cplayer is a desktop music player written in C++17. The project brings together graphics, audio metadata, and text-rendering technologies to build a native Windows application.

It is also a hands-on exploration of C++ application development, library integration, and building a custom desktop interface.

Features

* Desktop interface — Graphics powered by raylib.
* Audio metadata — Metadata handling through TagLib.
* Album artwork — Support for working with embedded cover art.
* Text rendering — Integration with FreeType, HarfBuzz, and SDL2_ttf.
* International text — Text-rendering technologies suitable for multilingual interfaces.
* Native build — CMake-based build system targeting Windows with MinGW-w64.

cplayer is under active development. Features and implementation details may change.

Screenshots

<!-- Add screenshots of the actual application below. -->

Main Window	Music Library
	

Built With

Technology	Role
C++17	Application development
raylib	Graphics
TagLib	Audio metadata
SDL2_ttf	Font rendering
FreeType	Font engine
HarfBuzz	Text shaping
libcurl	Networking
CMake	Build system
MinGW-w64	Windows toolchain

Building from Source

Prerequisites

* Windows
* MSYS2
* MinGW-w64 GCC/G++
* CMake
* The dependencies required by the project

1. Clone the repository

git clone https://github.com/kiracracker/cplayer.git
cd cplayer

2. Install dependencies

Open the MSYS2 MinGW64 terminal and install the available system libraries:

pacman -S --needed \
  mingw-w64-x86_64-curl \
  mingw-w64-x86_64-brotli \
  mingw-w64-x86_64-zlib \
  mingw-w64-x86_64-libpng \
  mingw-w64-x86_64-freetype

3. Configure the project

cmake -S . -B build -G "MinGW Makefiles"

4. Build

cmake --build build --parallel

The executable should be generated in the build directory if configuration and linking complete successfully.

Note: The current build configuration references C:/msys64/mingw64/ and includes prebuilt libraries. You may need to adjust dependency paths and library configuration for your environment.

Project Structure

cplayer/
├── fonts/          # Font assets
├── icons/          # Icons and UI assets
├── include/        # Headers
├── lib/            # Prebuilt libraries
├── src/            # C++ source files
├── CMakeLists.txt  # Build configuration
├── app_icon.ico    # Application icon
├── favicon.rc      # Windows resources
└── README.md

Development Status

cplayer is a personal project under development. The codebase and interface may evolve as new ideas are explored and the application grows.

Contributing

Suggestions, bug reports, and ideas are welcome. Feel free to open an issue to discuss a bug or a potential improvement.

License

No license information is currently specified in this README. Check the repository for an applicable license before redistributing the project.

⸻

<p align="center">
  Made by <a href="https://github.com/kiracracker">kiracracker</a>
</p>