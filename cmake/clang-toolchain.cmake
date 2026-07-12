set(CMAKE_CXX_COMPILER "/usr/bin/clang++-22" CACHE FILEPATH "Clang C++ compiler")

# Keep the complete C++ stack in LLVM: Clang, libc++, libc++abi and LLD.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
