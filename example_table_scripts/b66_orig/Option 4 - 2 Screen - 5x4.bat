CD /d %~dp0
del PinUpPlayer.ini
del pupinit.bat
del PuPDesktopPunch.exe
del PuPDesktopPunch-NoMinimize.exe
del "Option Selected.jpg"
del "Option Selected.txt"
echo off
xcopy /y "PuP-Pack_Options\Option 4 - 2 Screen - 5x4\*.pup"
xcopy /y "PuP-Pack_Options\Option 4 - 2 Screen - 5x4\Option Selected.jpg"
xcopy /y "PuP-Pack_Options\Option 4 - 2 Screen - 5x4\Option Selected.txt"
echo:
echo:
echo *************************************************************************
echo ********************* Option 4 - 2 Screen - 5x4/4x3 *********************
echo:
echo  - PuP-Pack "Option 4" files have been copied and are now being used
echo  - "Option Selected.jpg" shows you what the PuP-Pack should look like
echo:
echo ========== These table script options MUST be set (near the top) ==========
echo:
echo    DMDMode = 2
echo    bSingleScreen=false
echo    PuPDMDDriverType = 0
echo    b5x4Mode = True
echo:
echo ========== PUP "Backglass" Display ==========
echo:
echo  - pup Backglass display MUST be positioned on your backglass monitor
echo  - pup Backglass display MUST be set to a 5:4/4:3 ratio (1600x1200, 1280x1024, etc)
echo  - this is set using PinUpPlayerConfigDisplays.bat
echo:
echo *******************************************************************************
@pause
