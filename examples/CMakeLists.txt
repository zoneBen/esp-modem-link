# The examples are standalone ESP-IDF projects, built with idf.py through
# scripts/idf.ps1 - each has its own project() and pulls the library in as a
# component. They are deliberately NOT add_subdirectory() targets here: the host
# build compiles the library for POSIX and links the tools/ binaries, and an IDF
# project has no place in that graph. Nothing is registered in this file.
