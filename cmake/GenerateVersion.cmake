# Regenerates version.h at build time so the git revision and build date are
# always current. Invoked as:
#   cmake -DSRC=... -DDST=... -DPROTOBASE=... -P GenerateVersion.cmake
# configure_file only rewrites the output when the content changed, so an
# unchanged revision does not trigger a relink.

find_package(Git QUIET)
set(PROTO_GIT_REVISION "unknown")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty
        WORKING_DIRECTORY ${WORKDIR}
        OUTPUT_VARIABLE PROTO_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(PROTO_GIT_REVISION STREQUAL "")
        set(PROTO_GIT_REVISION "unknown")
    endif()
endif()

string(TIMESTAMP PROTO_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)

configure_file(${SRC} ${DST} @ONLY)
