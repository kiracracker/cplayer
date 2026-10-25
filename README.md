cplayer

A lightweight desktop music player built with C++17, focused on music playback, metadata, and a clean graphical interface.

Overview

cplayer is a personal music player project developed in C++ for Windows. It uses raylib for graphics and integrates several libraries for audio-related functionality, font rendering, and music metadata handling.

The project is also an opportunity to explore native desktop application development, multimedia libraries, and UI design in C++.

Features

* Music metadata — Read audio tags using TagLib.
* Album artwork — Support for handling embedded cover art.
* Graphical interface — A desktop UI built with raylib.
* Text rendering — Font rendering support using FreeType, HarfBuzz, and SDL2_ttf.
* Unicode support — Designed with international text rendering in mind, including Arabic.
* Native Windows application — Built using CMake and MinGW-w64.

Features and implementation details may change as development continues.

Built With

Technology	Purpose
C++17	Core application
raylib	Graphics and UI
TagLib	Audio metadata
SDL2_ttf	Font rendering
FreeType	Font engine
HarfBuzz	Text shaping
libcurl	Networking
CMake	Build configuration
MinGW-w64	Windows compilation toolchain

Requirements

To build cplayer from source, you will need:

* Windows
* MSYS2 with the MinGW-w64 toolchain
* GCC / G++
* CMake
* The project’s required libraries and development headers

The current build configuration uses the MSYS2 mingw64 environment and includes some prebuilt libraries in the repository.

Building from Source

1. Clone the repository

git clone https://github.com/kiracracker/cplayer.git
cd cplayer

2. Configure the project

Open the MSYS2 MinGW64 terminal and run:

cmake -S . -B build -G "MinGW Makefiles"

3. Build

cmake --build build --parallel

If the build succeeds, the executable will be generated in the build directory.

Note: The current CMake configuration references libraries under C:/msys64/mingw64/. If MSYS2 is installed elsewhere, or if required dependencies are missing, you may need to adjust the build configuration.

Project Structure

cplayer/
├── fonts/          # Font assets
├── icons/          # Application icons and UI assets
├── include/        # Headers and library headers
├── lib/            # Prebuilt libraries
├── src/            # Application source code
├── CMakeLists.txt  # CMake build configuration
├── favicon.rc      # Windows resource configuration
└── README.md

Development Status

cplayer is an ongoing personal project. Its features, interface, and internal architecture may evolve during development.

Contributions, suggestions, and bug reports are welcome.

License

No license has been specified yet. Please check this repository for licensing information before redistributing or modifying the project.

⸻

Developed by kiracracker.
