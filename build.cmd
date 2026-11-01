@echo off
setlocal
cd /d "%~dp0"
if defined VCToolsInstallDir goto build
rem vswhere trouve n'importe quelle edition (Community, Build Tools...).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo Visual Studio 2022 C++ Build Tools are required. Run from an x64 Native Tools prompt.
  exit /b 1
)
call "%VCVARS%" >nul
:build
if not exist build mkdir build
set "SCANFOLDER_OUTPUT=ScanFolder.exe"
if not "%~1"=="" set "SCANFOLDER_OUTPUT=%~1"
rc /nologo /fo build\app.res app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /GL /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /Fo:build\ /Fe:"%SCANFOLDER_OUTPUT%" src\app.cpp src\scan.cpp src\mft.cpp src\delete.cpp src\view.cpp src\operations.cpp src\archive.cpp src\cache.cpp src\store.cpp src\tests.cpp build\app.res /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
if errorlevel 1 exit /b 1
echo Built %SCANFOLDER_OUTPUT%
endlocal
