# Regenerates version.h at build time. Invoked per build as:
#   cmake -DSRC=... -DDST=... -DBASE=... -DWORKDIR=... -P GenerateVersion.cmake
#
# The build number auto-increments on every build, persisted in the buildnum
# file at the source root (the modern descendant of mkversion.sh, which kept
# the counter inside version.c). PROTOBASE is composed as "<BASE>.<number>".
# Git revision, build date, and the build host's uname ride along as sub-info.

set(BUILDNUM_FILE ${WORKDIR}/buildnum)
set(PROTO_BUILD_NUMBER 1)
if(EXISTS ${BUILDNUM_FILE})
    file(READ ${BUILDNUM_FILE} _n)
    string(STRIP "${_n}" _n)
    if(_n MATCHES "^[0-9]+$")
        math(EXPR PROTO_BUILD_NUMBER "${_n} + 1")
    endif()
endif()
file(WRITE ${BUILDNUM_FILE} "${PROTO_BUILD_NUMBER}\n")

set(PROTOBASE "${BASE}.${PROTO_BUILD_NUMBER}")

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
string(TIMESTAMP PROTO_CREATION "%a %b %d %Y at %H:%M:%S UTC" UTC)

set(UNAME_VALUE "unknown")
execute_process(COMMAND uname -a
    OUTPUT_VARIABLE UNAME_VALUE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

configure_file(${SRC} ${DST} @ONLY)
