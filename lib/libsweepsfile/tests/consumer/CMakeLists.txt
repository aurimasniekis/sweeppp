# A consumer of the *installed* package, not of the source tree.
#
# Three lines, deliberately: find_package, add_executable, target_link_libraries.
# If that does not work, the library is not installable, whatever the build tree
# says. This is the claim the whole extraction rests on, so it is tested rather
# than asserted -- and it has already caught one real defect, a missing
# target_compile_features that left consumers compiling the C++17 headers at
# whatever standard their compiler defaulted to.

cmake_minimum_required(VERSION 3.24)
project(sweeps_consumer LANGUAGES CXX)

find_package(sweepsfile REQUIRED)

add_executable(sweeps-consumer main.cpp)
target_link_libraries(sweeps-consumer PRIVATE sweeps::sweepsfile)
