CD /d %~dp0
del PinUpPlayer.ini
del pupinit.bat
del PuPDesktopPunch.exe
del PuPDesktopPunch-NoMinimize.exe
del "Option Selected.jpg"
del "Option Selected.txt"
echo off
xcopy /y "PuP-Pack_Options\Option 2 - 2 Screen - 16x9\*.pup"
xcopy /y "PuP-Pack_Options\Option 2 - 2 Screen - 16x9\Option Selected.jpg"
xcopy /y "PuP-Pack_Options\Option 2 - 2 Screen - 16x9\Option Selected.txt"
echo:
echo:
echo *************************************************************************
echo ********************* Option 2 - 2 Screen - 16x9 ************************
echo:
echo  - PuP-Pack "Option 2" files have been copied and are now being used
echo  - "Option Selected.jpg" shows you what the PuP-Pack should look like
echo:
echo ========== These table script options MUST be set (near the top) ==========
echo:
echo    DMDMode = 2
echo    bSingleScreen=false
echo    PuPDMDDriverType = 0
echo:
echo ========== PUP "Backglass" Display ==========
echo:
echo  - pup Backglass display MUST be positioned on your backglass monitor
echo  - pup Backglass display MUST be set to a 16:9 ratio (1920x1080, 1366x768, 1280x720, etc)
echo  - this is set using PinUpPlayerConfigDisplays.bat
echo:
echo *******************************************************************************
@pause
