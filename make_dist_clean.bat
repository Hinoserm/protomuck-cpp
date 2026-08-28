@echo off
REM Cleaner - Scimicat

echo This batch file is for cleaning up the directories, be careful!
echo.
echo C - Regular clean : Removes objs, pdbs, exes, user files
echo S - Squeaky clean : Removes everything, including libraries
REM echo P - Make packaged : Like S, but zips up the directory too
echo Q - Exits
echo. 

:makemychoice
set /P c=What would you like to do?
if /I "%c%" EQU "C" goto :regularclean
if /I "%c%" EQU "S" goto :squeakyclean
REM if /I "%c%" EQU "P" goto :makepackaged
if /I "%c%" EQU "Q" goto :escapefromit
goto :makemychoice

:makepackaged
echo. 
:choiceoneandahalf
set /P c=Are you absolutely sure you want a clean package [Y/N]?
if /I "%c%" EQU "Y" goto :setuptopkgit
if /I "%c%" EQU "N" goto :escapefromit
goto :choiceoneandahalf

:setuptopkgit
set PKGIT=true
goto :escapefromit

:squeakyclean
echo. 
:choicetwo
set /P c=Are you absolutely sure you want to delete libs [Y/N]?
if /I "%c%" EQU "Y" goto :killlthelibs
if /I "%c%" EQU "N" goto :escapefromit
goto :choicetwo

:regularclean
del game\proto2.* /Q
del game\*.dll /Q
goto :continueclen

:killlthelibs
rmdir /Q/s ext\x86
rmdir /Q/s game
del prj\proto2.vcxproj.user 
del proto2.sdf 
del proto2.opensdf 
del proto2.v11.suo /A:H

mkdir game
robocopy dat game /purge /s /njh /njs /ndl /ndl /nc /ns /np /XX

:continueclen
del obj\*.* /Q

pause 
exit

:escapefromit
echo Ok, exiting without doing anything.
pause
exit