cmake_minimum_required(VERSION 3.16)

project(protomuck
    VERSION 2.1.0
    DESCRIPTION "ProtoMUCK server, C++ line"
    LANGUAGES CXX)

# Version base; the build number is appended by GenerateVersion.cmake, which
# auto-increments the buildnum file at the source root on every build.
set(PROTO_VERSION_BASE "2.1a0.6")

# NOTE: build configuration is currently BAKED in src/inc/autoconf.h, carried
# over from the last proven autoconf run: SSL, MySQL, PCRE, and zlib enabled;
# IPv6 and loadable modules disabled (both code paths have bit-rotted and do
# not compile). Proper feature detection can replace this later.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(
    -Wall
    -Wno-write-strings
    -Wno-unused-result
    -fstack-protector-all)

# --- dependencies (all required, matching the baked config) ---
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_path(PCRE_INCLUDE_DIR pcre.h REQUIRED)
find_library(PCRE_LIBRARY pcre REQUIRED)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h REQUIRED)
find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb REQUIRED)

# --- generated version.h: refreshed every build with git revision and date ---
set(GEN_INC ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_INC})
add_custom_command(
    OUTPUT ${GEN_INC}/version.h __always_regenerate
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/cmake/version.h.in
        -DDST=${GEN_INC}/version.h
        -DBASE=${PROTO_VERSION_BASE}
        -DWORKDIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake
    COMMENT "Generating version.h")
add_custom_target(gen_version DEPENDS ${GEN_INC}/version.h)

# --- sources: mirrors the old Makefile.in COBJ list plus interface.cpp ---
set(PROTO_SOURCES
    src/array.cpp src/boolexp.cpp src/cgi.cpp src/compile.cpp src/compress.cpp
    src/create.cpp src/Database.cpp src/MacroTable.cpp src/UUID.cpp src/DbObject.cpp src/LegacyDbIO.cpp src/ModuleRegistry.cpp src/ObjectStore.cpp src/ObjectAccess.cpp src/Journal.cpp src/FlatFileConverter.cpp src/PasswordHash.cpp src/ProgramStore.cpp src/PropertyTree.cpp src/PropNode.cpp src/debugger.cpp src/disassem.cpp
    src/edit.cpp src/events.cpp src/game.cpp src/hashtab.cpp
    src/help.cpp src/inst.cpp src/interp.cpp src/log.cpp src/look.cpp
    src/match.cpp src/mfuns.cpp src/mfuns2.cpp src/mfuns3.cpp src/msgparse.cpp
    src/move.cpp src/mufevent.cpp src/crt_malloc.cpp
    src/player.cpp src/predicates.cpp src/p_array.cpp src/p_connects.cpp
    src/p_db.cpp src/p_error.cpp src/p_float.cpp src/p_file.cpp src/p_html.cpp
    src/p_http.cpp src/p_math.cpp src/p_misc.cpp src/p_muf.cpp src/p_mysql.cpp
    src/p_props.cpp src/p_socket.cpp src/p_stack.cpp src/p_strings.cpp
    src/p_system.cpp src/p_regex.cpp src/property.cpp src/propdirs.cpp
    src/props.cpp src/random.cpp src/rob.cpp src/set.cpp src/signal.cpp
    src/smatch.cpp src/speech.cpp src/strftime.cpp src/stringutil.cpp
    src/timequeue.cpp src/timestamp.cpp src/tune.cpp src/unparse.cpp
    src/utils.cpp src/wiz.cpp src/reg.cpp src/netresolve.cpp src/sanity.cpp
    src/newhttp.cpp src/version.cpp src/sha1.cpp
    src/interface.cpp)

add_executable(protomuck ${PROTO_SOURCES})
# export our symbols so dlopened feature modules resolve against them
set_target_properties(protomuck PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(protomuck gen_version)
target_include_directories(protomuck BEFORE PRIVATE
    ${GEN_INC} src/inc ${PCRE_INCLUDE_DIR} ${MYSQL_INCLUDE_DIR})
target_link_libraries(protomuck PRIVATE
    Threads::Threads ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto
    ${PCRE_LIBRARY} ${MYSQL_LIBRARY} ${CMAKE_DL_LIBS})

add_executable(resolver src/resolver.cpp)
add_dependencies(resolver gen_version)
target_include_directories(resolver BEFORE PRIVATE ${GEN_INC} src/inc)
target_link_libraries(resolver PRIVATE Threads::Threads)

install(TARGETS protomuck resolver
    RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/game)
