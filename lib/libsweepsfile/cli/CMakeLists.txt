# The `sweeps` command line.
#
# Split into a static core plus a ten-line binary on purpose. The core's entry
# points take `std::ostream&` rather than printing, which is what lets Sweep++'s
# own CLI -- a C++23 binary that formats with std::println -- call into this
# C++17 library instead of carrying a second implementation of the same output.
#
# The core is deliberately not installed: it is an implementation detail shared
# by two executables, not part of the library's interface.

add_library(sweepsfile-cli-core STATIC
    Commands.cpp
)

add_library(sweeps::cli-core ALIAS sweepsfile-cli-core)

set_target_properties(sweepsfile-cli-core PROPERTIES
    CXX_STANDARD ${SWEEPSFILE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_include_directories(sweepsfile-cli-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(sweepsfile-cli-core PUBLIC sweeps::sweepsfile)

if(NOT MSVC)
    target_compile_options(sweepsfile-cli-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(sweeps main.cpp)

set_target_properties(sweeps PROPERTIES
    CXX_STANDARD ${SWEEPSFILE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_link_libraries(sweeps PRIVATE sweeps::cli-core)

if(NOT MSVC)
    target_compile_options(sweeps PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Inside a Sweep++ build the binary joins everything else in dist/, so it sits
# beside sweeppp-cli rather than somewhere a developer has to go looking.
if(DEFINED SWEEPPP_OUTPUT_DIR)
    set_target_properties(sweeps PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SWEEPPP_OUTPUT_DIR}")
endif()

if(SWEEPSFILE_INSTALL)
    install(TARGETS sweeps RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
