CD /d %~dp0
del "Option Selected.jpg"
del "Option Selected.txt"
echo off
xcopy /y "PuP-Pack_Options\Option 3 - Desktop\*.*"
echo:
echo:
echo ****************************************************************************
echo ********************* Option 3 - Desktop - FullDMD *************************
echo:
echo  - PuP-Pack "Option 3" files have been copied and are now being used
echo  - "Option Selected.jpg" shows you what the PuP-Pack should look like
echo:
echo ========== These table script options MUST be set (near the top) ==========
echo:
echo    DMDMode = 2
echo    bSingleScreen=true
echo    PuPDMDDriverType = 0
echo:
echo ========== VPX Borderless Windowed Mode ==========
echo:
echo  - you MUST have VPX set to Borderless Windowed Mode
echo  - this is needed so PUPDesktopPunch can allow the PuP-Pack appear over the VPX Window
echo  - when table loads, wait about 10 to 15 seconds for entire PuP-Pack to appear before starting a game
echo:
echo ****************************************************************************
@pause