@echo off
rem Fuzzes the byte-facing parsers. The first word picks the target (trf, protocol, chunk
rem or all); everything else goes to libFuzzer unchanged:
rem   fuzz.bat                                        trf, one worker, 60 seconds
rem   fuzz.bat protocol -jobs=4 -workers=4
rem   fuzz.bat chunk -runs=2000 -print_final_stats=1
rem   fuzz.bat all -max_total_time=120
rem Crashes, timeouts and leaks are written to out\fuzz-corpus\<target>\crashes.
rem %* is used for the arguments because %1 is split at '=' by cmd, which would turn
rem "-runs=300" into two arguments.
setlocal

set "TARGET=trf"
set "ARGS=%*"
set "TARGET_WORD="
if /i "%~1"=="trf"      set "TARGET_WORD=trf"
if /i "%~1"=="protocol" set "TARGET_WORD=protocol"
if /i "%~1"=="chunk"    set "TARGET_WORD=chunk"
if /i "%~1"=="all"      set "TARGET_WORD=all"

if defined TARGET_WORD (
    set "TARGET=%TARGET_WORD%"
    call :strip_target
)

if /i "%TARGET%"=="all" (
    call "%~f0" trf %ARGS%
    call "%~f0" protocol %ARGS%
    call "%~f0" chunk %ARGS%
    exit /b 0
)

set "EXE=%~dp0out\build\x64-fuzz\fuzz_%TARGET%.exe"
if /i "%TARGET%"=="trf" set "EXE=%~dp0out\build\x64-fuzz\fuzz_trf_decode.exe"
if not exist "%EXE%" (
    echo %EXE% not found, build the fuzzers first:
    echo   cmake --preset x64-fuzz
    echo   cmake --build --preset x64-fuzz
    exit /b 1
)

set "CORPUS=%~dp0out\fuzz-corpus\%TARGET%"
set "CRASHES=%CORPUS%\crashes"
if not exist "%CORPUS%" mkdir "%CORPUS%"
if not exist "%CRASHES%" mkdir "%CRASHES%"

rem Run inside the corpus folder: -jobs writes fuzz-0.log, fuzz-1.log, ... beside the
rem working directory, and out/ is already ignored by git.
pushd "%CORPUS%"
if "%ARGS%"=="" (
    "%EXE%" "%CORPUS%" -artifact_prefix=%CRASHES%\ -max_total_time=60 -print_final_stats=1
) else (
    "%EXE%" "%CORPUS%" -artifact_prefix=%CRASHES%\ %ARGS%
)
set "STATUS=%ERRORLEVEL%"
popd

if not "%STATUS%"=="0" (
    echo.
    echo fuzz_%TARGET% stopped with exit code %STATUS%; failing inputs are in:
    echo   %CRASHES%
    dir /b "%CRASHES%"
)
exit /b %STATUS%

:strip_target
if /i "%ARGS%"=="%TARGET_WORD%" (
    set "ARGS="
    exit /b 0
)
if /i "%TARGET_WORD%"=="trf"      set "ARGS=%ARGS:*trf =%"
if /i "%TARGET_WORD%"=="protocol" set "ARGS=%ARGS:*protocol =%"
if /i "%TARGET_WORD%"=="chunk"    set "ARGS=%ARGS:*chunk =%"
if /i "%TARGET_WORD%"=="all"      set "ARGS=%ARGS:*all =%"
exit /b 0
