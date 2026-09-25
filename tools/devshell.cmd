@echo off
rem Rack Network Fabric - Windows developer shell helper.
rem Locates Visual Studio via vswhere, imports the x64 developer environment,
rem then executes the command passed as arguments. Non-Windows users should use
rem tools/devshell.sh instead.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [devshell] vswhere.exe not found; cannot locate Visual Studio. 1>&2
  exit /b 127
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [devshell] no Visual Studio installation with the C++ toolset was found. 1>&2
  exit /b 127
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [devshell] failed to initialize the x64 developer environment. 1>&2
  exit /b 127
)
%*
exit /b %ERRORLEVEL%
