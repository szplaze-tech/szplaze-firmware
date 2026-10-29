@echo off
REM ============================================================
REM  One-click GitHub setup for Keil/STM32 project
REM  Usage:
REM    1. Copy this file into your project ROOT folder
REM       (the folder that contains your .uvprojx or source code)
REM    2. Double-click it, follow the prompts
REM  It will:
REM    - init git (GBK-safe, no line-ending conversion)
REM    - set git user name / email if missing
REM    - create .gitignore (auto, skips build junk and _bak_ folders)
REM    - create CI workflow (disabled by default, enable later)
REM    - commit ALL project files and push to your GitHub repo
REM
REM  Network note: SSH port 22 is blocked on this machine.
REM  Use HTTPS repo URL + Personal Access Token, not git@github.com.
REM  Example: https://github.com/szplaze-tech/szplaze-firmware.git
REM ============================================================
setlocal

echo.
echo  =========== GitHub one-click setup ===========
echo.

where git >nul 2>nul
if errorlevel 1 goto nogit

REM ---- 1. git init ----
if exist .git goto skipinit
git init -b main
echo [1/5] Git repo initialized. Branch: main.
goto afterinit
:skipinit
echo [1/5] Git repo already initialized, kept as is.
:afterinit

REM ---- 1b. identity and encoding (always enforced) ----
git config core.autocrlf false
git config core.quotepath false
if not "%GIT_NAME%"=="" goto skipname
for /f "tokens=*" %%i in ('git config --global user.name') do set GIT_NAME=%%i
:skipname
if "%GIT_NAME%"=="" set /p GIT_NAME=Your name for commits:
git config user.name "%GIT_NAME%"
if "%GIT_EMAIL%"=="" set /p GIT_EMAIL=Your company email:
git config user.email "%GIT_EMAIL%"
echo       Commit author: %GIT_NAME% ^<%GIT_EMAIL%^>
echo       Line endings: no conversion, UTF-8 safe for GBK sources.

REM ---- 2. .gitignore ----
if exist .gitignore goto skipignore
call :write_gitignore
echo [2/5] .gitignore created.
goto afterignore
:skipignore
echo [2/5] .gitignore already exists, kept as is.
:afterignore

REM ---- 3. CI workflow ----
if not exist .github\workflows mkdir .github\workflows
if exist .github\workflows\build-release-keil.yml goto skipwf
call :write_workflow
echo [3/5] CI workflow created. It is DISABLED by default.
echo       After you register a Keil runner, open the yml and
echo       delete the '#' on the 'push/tags' lines to enable it.
goto afterwf
:skipwf
echo [3/5] CI workflow already exists, kept as is.
:afterwf

REM ---- 4. ask repo url ----
echo.
echo [4/5] Paste your GitHub repo HTTPS URL, then press Enter.
echo       Example: https://github.com/szplaze-tech/szplaze-firmware.git
echo       Do NOT use git@github.com - SSH port 22 is blocked here.
echo.
set REPO_URL=
set /p REPO_URL=Repo URL:
if "%REPO_URL%"=="" goto ask

REM ---- 5. commit and push ----
git remote remove origin >nul 2>nul
git remote add origin "%REPO_URL%"
echo Adding all project files...
git add -A
git commit -m "Initial commit: full project"
echo.
echo [5/5] Pushing to GitHub (first push may take a while)...
git push -u origin main
if errorlevel 1 goto pushfail

echo.
echo  =========== DONE ===========
echo  Your whole project is now on GitHub.
echo  Open the repo page in your browser to verify.
echo.
pause
exit /b 0

:nogit
echo [ERROR] git not found. Install it from:
echo         https://git-scm.com/download/win
pause
exit /b 1

:pushfail
echo.
echo [ERROR] Push failed. Common causes:
echo  1. Authentication failed - you need a Personal Access Token, not a password.
echo     github.com - avatar - Settings - Developer settings
echo       - Personal access tokens - Tokens classic - Generate new token
echo     Check the 'repo' scope, copy the token, and when git asks for
echo     'Password' paste the TOKEN instead.
echo  2. Wrong repo URL - must be https://github.com/OWNER/REPO.git
echo  3. Remote already has commits - this script expects an EMPTY repo.
echo.
echo  Tip: cache the token so you only type it once:
echo     git config --global credential.helper manager
pause
exit /b 1

:write_gitignore
>>.gitignore echo # Keil MDK / STM32 project - keep sources, skip build junk
>>.gitignore echo *.o
>>.gitignore echo *.d
>>.gitignore echo *.crf
>>.gitignore echo *.lst
>>.gitignore echo *.htm
>>.gitignore echo *.dep
>>.gitignore echo *.iex
>>.gitignore echo *.lnp
>>.gitignore echo *.axf
>>.gitignore echo *.elf
>>.gitignore echo *.uvguix.*
>>.gitignore echo *.bak
>>.gitignore echo *.log
>>.gitignore echo Objects/
>>.gitignore echo Listings/
>>.gitignore echo RTE/
>>.gitignore echo DebugConfig/
>>.gitignore echo Debug/
>>.gitignore echo Release/
>>.gitignore echo .settings/
>>.gitignore echo _bak_*
>>.gitignore echo *backup*
>>.gitignore echo JLinkLog.txt
>>.gitignore echo *.jlink
>>.gitignore echo EventRecorderStub.scvd
>>.gitignore echo Thumbs.db
>>.gitignore echo desktop.ini
>>.gitignore echo .vscode/
goto :eof

:write_workflow
>>.github\workflows\build-release-keil.yml echo name: Keil Build and Release
>>.github\workflows\build-release-keil.yml echo on:
>>.github\workflows\build-release-keil.yml echo   workflow_dispatch
>>.github\workflows\build-release-keil.yml echo   # push:
>>.github\workflows\build-release-keil.yml echo   #   tags: [ "v*" ]
>>.github\workflows\build-release-keil.yml echo jobs:
>>.github\workflows\build-release-keil.yml echo   build:
>>.github\workflows\build-release-keil.yml echo     runs-on: [ self-hosted, keil ]
>>.github\workflows\build-release-keil.yml echo     steps:
>>.github\workflows\build-release-keil.yml echo       - name: Checkout
>>.github\workflows\build-release-keil.yml echo         uses: actions/checkout@v4
>>.github\workflows\build-release-keil.yml echo       - name: Keil command line build
>>.github\workflows\build-release-keil.yml echo         shell: cmd
>>.github\workflows\build-release-keil.yml echo         run: ^|
>>.github\workflows\build-release-keil.yml echo           "C:\Keil_v5\UV4\UV4.exe" -b MDK-ARM\Project.uvprojx -j0 -o build_log.txt
>>.github\workflows\build-release-keil.yml echo           type MDK-ARM\build_log.txt
>>.github\workflows\build-release-keil.yml echo           if errorlevel 1 exit /b 1
>>.github\workflows\build-release-keil.yml echo       - name: Collect artifacts
>>.github\workflows\build-release-keil.yml echo         shell: cmd
>>.github\workflows\build-release-keil.yml echo         run: ^|
>>.github\workflows\build-release-keil.yml echo           mkdir release
>>.github\workflows\build-release-keil.yml echo           for /r MDK-ARM %%f in (*.hex *.bin *.axf) do copy "%%f" release\
>>.github\workflows\build-release-keil.yml echo       - name: Create Release and upload
>>.github\workflows\build-release-keil.yml echo         uses: softprops/action-gh-release@v2
>>.github\workflows\build-release-keil.yml echo         with:
>>.github\workflows\build-release-keil.yml echo           files: release/*
>>.github\workflows\build-release-keil.yml echo         env:
>>.github\workflows\build-release-keil.yml echo           GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
goto :eof
