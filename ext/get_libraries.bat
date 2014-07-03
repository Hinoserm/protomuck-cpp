@echo off
REM https://openssl-for-windows.googlecode.com/files/openssl-0.9.8k_WIN32.zip
REM http://www.psyon.org/projects/pcre-win32/pcre-7.9-static.zip
REM http://cdn.mysql.com/Downloads/Connector-C/mysql-connector-c-6.1.0-win32.zip

mkdir x86
mkdir x86\dll
mkdir x86\inc
mkdir x86\lib
mkdir x86\pdb

echo Obtaining the x86 libraries...
pause

mkdir tmp
cd tmp
echo Setting up OPENSSL 0.9.8k libraries....
echo --- Downloading ---
..\bin\wget http://openssl-for-windows.googlecode.com/files/openssl-0.9.8k_WIN32.zip
echo --- Extracting ---
..\bin\7za x -y openssl-0.9.8k_WIN32.zip
echo --- Moving ---
move /Y bin\*.dll ..\x86\dll\
move /Y include\openssl ..\x86\inc
move /Y lib\*.lib ..\x86\lib\

cd ..
rmdir /Q/S tmp
mkdir tmp
cd tmp

echo Setting up PCRE 7.9 libraries....
echo --- Downloading ---
..\bin\wget http://www.psyon.org/projects/pcre-win32/pcre-7.9-dll.zip
echo --- Extracting ---
..\bin\7za x -y pcre-7.9-dll.zip
echo --- Moving ---
move pcre-7.9-dll\pcre.lib ..\x86\lib\
move pcre-7.9-dll\pcre.dll ..\x86\dll\
move pcre-7.9-dll\*.* ..\x86\inc\

cd ..
rmdir /Q/S tmp
mkdir tmp
cd tmp

echo Setting up MYSQL 6.1.0 libraries....
echo --- Downloading ---
..\bin\wget http://cdn.mysql.com/Downloads/Connector-C/mysql-connector-c-6.1.5-win32.zip
echo --- Extracting ---
..\bin\7za x -y mysql-connector-c-6.1.5-win32.zip
echo --- Moving ---
move mysql-connector-c-6.1.5-win32\lib\*.lib ..\x86\lib
move mysql-connector-c-6.1.5-win32\lib\*.pdb ..\x86\pdb
move mysql-connector-c-6.1.5-win32\lib\debug\mysqlclient.lib ..\x86\lib\mysqlclient_d.lib
move mysql-connector-c-6.1.5-win32\lib\debug\mysqlclient.pdb ..\x86\pdb\mysqlclient_d.pdb
move mysql-connector-c-6.1.5-win32\lib\*.dll ..\x86\dll
mkdir ..\x86\inc\mysql
move mysql-connector-c-6.1.5-win32\include\*.* ..\x86\inc\mysql
move mysql-connector-c-6.1.5-win32\include\mysql ..\x86\inc\mysql

cd ..
rmdir /Q/S tmp
mkdir tmp
cd tmp

echo Setting up ZLIB 1.2.8 libraries...
echo --- Downloading ---
..\bin\wget http://zlib.net/zlib128-dll.zip
echo --- Extracting ---
..\bin\7za x -y zlib128-dll.zip
echo --- Moving ---
move include\*.h ..\x86\inc
move lib\zdll.lib ..\x86\lib\zlib.lib
move lib\zlib.def ..\x86\lib\zlib.def
move zlib1.dll ..\x86\dll

cd ..
rmdir /Q/S tmp

copy 
goto :end

REM Going to be adding support for x64 windows compilation in Visual Studio
REM Problem is, its looking like I have to compile it all myself, so I'll 
REM return to this in a bit. Built PCRE8.33 already, hosted on google drive
REM Can't find zlib VC11 x64, so before I go further, I'd rather wait until
REM everything else is in place. Get one working before we do both, eh?

mkdir tmp
cd tmp

echo Obtaining x64 libraries second, this will likely take longer..

echo Setting up PCRE 8.33 x64 libraries...
echo --- Downloading ---
..\bin\wget --no-check-certificate "http://googledrive.com/host/0B0DHNT_skG3hMlhpbmkyeVZ1SW8" -O pcre-8.33-VC11-x64.7z
echo --- Extracting ---
..\bin\7za x -y pcre-8.33-VC11-x64.7z
echo --- Moving ---

:end
echo Skipping x64 libraries (read the comment in the batch about why not, yet)
echo Done setting up libraries. 
pause
