@echo off
REM "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\bin\x86_amd64\vcvarsx86_amd64.bat"
mkdir PACKAGE
copy pcre.h.generic pcre.h
echo #ifndef SUPPORT_UTF8 > config.h
echo #define SUPPORT_UTF8 >> config.h
echo #endif >> config.h
echo #ifndef SUPPORT_UCP >> config.h
echo #define SUPPORT_UCP >> config.h
echo #endif >> config.h
echo #define HAVE_STRERROR 1 >> config.h
echo #define HAVE_MEMMOVE 1 >> config.h
echo #ifndef NEWLINE >> config.h
echo #define NEWLINE '\n' >> config.h
echo #endif >> config.h
type config.h.generic >> config.h
cl -DHAVE_CONFIG_H dftables.c
dftables.exe pcre_chartables.c
cl -DHAVE_CONFIG_H /c pcre_chartables.c pcre_compile.c pcre_config.c pcre_dfa_exec.c pcre_exec.c pcre_fullinfo.c pcre_get.c pcre_globals.c pcre_maketables.c pcre_newline.c pcre_ord2utf8.c pcre_refcount.c pcre_study.c pcre_tables.c pcre_ucd.c pcre_valid_utf8.c pcre_version.c pcre_xclass.c
link /DLL /OUT:pcre.dll pcre_chartables.obj pcre_compile.obj pcre_config.obj pcre_dfa_exec.obj pcre_exec.obj pcre_fullinfo.obj pcre_get.obj pcre_globals.obj pcre_maketables.obj pcre_newline.obj pcre_ord2utf8.obj pcre_refcount.obj pcre_study.obj pcre_tables.obj pcre_ucd.obj pcre_valid_utf8.obj pcre_version.obj pcre_xclass.obj
mkdir PACKAGE\dll
copy pcre.dll PACKAGE\dll\pcre.dll
copy pcre.lib PACKAGE\dll\pcre.lib
copy pcre.exp PACKAGE\dll\pcre.exp
cl -DHAVE_CONFIG_H /c pcreposix.c
link /DLL /OUT:pcreposix.dll pcreposix.obj pcre.lib
copy pcreposix.dll PACKAGE\DLL\pcreposix.dll
copy pcreposix.lib PACKAGE\DLL\pcreposix.lib
copy pcreposix.exp PACKAGE\DLL\pcreposix.exp
REM cl -DHAVE_CONFIG_H pcretest.c pcre.lib pcreposix.lib
cl -DHAVE_CONFIG_H pcregrep.c pcre.lib
copy config.h PACKAGE\DLL
echo #define PCRE_STATIC 1 >> config.h
cl -DHAVE_CONFIG_H /c pcre_chartables.c pcre_compile.c pcre_config.c pcre_dfa_exec.c pcre_exec.c pcre_fullinfo.c pcre_get.c pcre_globals.c pcre_maketables.c pcre_newline.c pcre_ord2utf8.c pcre_refcount.c pcre_study.c pcre_tables.c pcre_ucd.c pcre_valid_utf8.c pcre_version.c pcre_xclass.c
lib /OUT:pcre.lib pcre_chartables.obj pcre_compile.obj pcre_config.obj pcre_dfa_exec.obj pcre_exec.obj pcre_fullinfo.obj pcre_get.obj pcre_globals.obj pcre_maketables.obj pcre_newline.obj pcre_ord2utf8.obj pcre_refcount.obj pcre_study.obj pcre_tables.obj pcre_ucd.obj pcre_valid_utf8.obj pcre_version.obj pcre_xclass.obj
cl -DHAVE_CONFIG_H /c pcreposix.c
lib /OUT:pcreposix.lib pcreposix.obj
mkdir PACKAGE\static
copy pcre.lib PACKAGE\static\pcre.lib
copy pcreposix.lib PACKAGE\static\pcreposix.lib
copy config.h PACKAGE\static
mkdir PACKAGE\include
copy *.h PACKAGE\include
del PACKAGE\include\config.h
copy LICENSE PACKAGE
copy COPYING PACKAGE
echo Built from official 8.33 source for the Windows VC++ x64 platform. > PACKAGE\README
type README >> PACKAGE\README