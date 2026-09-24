@echo off
rem Right-click → Run as administrator to install Comfy Router into DaVinci Resolve.
set "DEST=%CommonProgramFiles%\OFX\Plugins"
echo Installing Comfy Router to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\ComfyRouter.ofx.bundle" rmdir /s /q "%DEST%\ComfyRouter.ofx.bundle"
xcopy /E /I /Y "%~dp0ComfyRouter.ofx.bundle" "%DEST%\ComfyRouter.ofx.bundle" >nul
if errorlevel 1 (
  echo Copy failed - right-click this file and choose "Run as administrator".
) else (
  echo Done. Restart DaVinci Resolve, then find it under OpenFX ^> Comfy ^> Comfy Router.
)
pause
