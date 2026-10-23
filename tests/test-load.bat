@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: Locate project root and compiler.
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT_DIR=%%~fI"

if exist "%ROOT_DIR%\build\bin\Release\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\Release\clx.exe"
) else if exist "%ROOT_DIR%\build\Release\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\Release\clx.exe"
) else if exist "%ROOT_DIR%\build\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\clx.exe"
) else if exist "%ROOT_DIR%\build\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\clx.exe"
) else if exist "%ROOT_DIR%\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\bin\clx.exe"
) else (
    echo Error: clx.exe not found. Run build.bat first.
    exit /b 1
)

set "TMP_DIR=%TEMP%\clx-load-test-%RANDOM%"
set "BIN=%TMP_DIR%\run_via_load.exe"
set "LOG=%TMP_DIR%\run_via_load.log"

mkdir "%TMP_DIR%" >nul 2>&1
if not exist "%TMP_DIR%" (
    echo [FAIL] Could not create temporary directory: %TMP_DIR%
    exit /b 1
)

cd /d "%ROOT_DIR%"
if errorlevel 1 (
    echo [FAIL] Could not change to project root: %ROOT_DIR%
    goto :fail
)

echo Using compiler: %COMPILER%
echo Compiling tests\run_via_load.lua with --dynamic...
"%COMPILER%" --dynamic tests\run_via_load.lua --output "%BIN%"
if errorlevel 1 goto :compile_fail
if not exist "%BIN%" goto :compile_fail

echo Running load-mode test suite...
"%BIN%" >"%LOG%" 2>&1
set "RUN_EXIT=!errorlevel!"
type "%LOG%"

:: run_via_load.lua reports aggregate failures in its summary. Check the
:: summary as well as the process status because older harness versions did
:: not propagate fail_count through their exit status.
findstr /c:"fail  : 0" "%LOG%" >nul 2>&1
if !RUN_EXIT! neq 0 goto :harness_fail
if !errorlevel! neq 0 goto :harness_fail
echo [PASS] load-mode test suite
goto :dump_test

:harness_fail
echo [FAIL] load-mode test suite
if !RUN_EXIT! neq 0 echo Harness exit code: !RUN_EXIT!
goto :fail

:: AOT-side string.dump coverage: the same test the default suite runs bare,
:: compiled with --dynamic so the VM round-trip section executes with a real
:: AOT caller on the clx side of the bridge.
:dump_test
echo Compiling tests\conformance\string_dump.lua with --dynamic...
set "DUMP_BIN=%TMP_DIR%\dump_dynamic.exe"
set "DUMP_LOG=%TMP_DIR%\dump_dynamic.log"
"%COMPILER%" --dynamic tests\conformance\string_dump.lua --output "%DUMP_BIN%"
if errorlevel 1 (
    echo [FAIL] Could not compile the AOT-side dump test.
    goto :fail
)
"%DUMP_BIN%" >"%DUMP_LOG%" 2>&1
set "DUMP_EXIT=!errorlevel!"
type "%DUMP_LOG%"
findstr /c:"[FAIL]" "%DUMP_LOG%" >nul 2>&1
if !DUMP_EXIT! neq 0 goto :dump_fail
if !errorlevel! equ 0 goto :dump_fail
echo [PASS] AOT-side string.dump (--dynamic)
goto :success

:dump_fail
echo [FAIL] AOT-side string.dump (--dynamic)
goto :fail

:compile_fail
echo [FAIL] Could not compile the load-mode test harness.
goto :fail

:success
rmdir /s /q "%TMP_DIR%" >nul 2>&1
exit /b 0

:fail
rmdir /s /q "%TMP_DIR%" >nul 2>&1
exit /b 1
