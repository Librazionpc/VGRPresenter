@echo off
rem Starts the INSTALLED VGRPresenter with full logging (deployment mode off), whatever build it is.
rem The log is written to <install folder>\logs\engine.log (or %AppData%\VGR\VGRPresenter\logs if that folder is not writable).
rem Use it for one run: reproduce the problem, close the app, then send / read engine.log.
set VGR_DEPLOY=0
set "VGR_APP=%LOCALAPPDATA%\Programs\VGRPresenter\appVGRPresenterUI.exe"
if not exist "%VGR_APP%" (
    echo VGRPresenter is not installed at %VGR_APP%
    pause
    exit /b 1
)
start "" "%VGR_APP%"
echo Started with full logging. Log: %LOCALAPPDATA%\Programs\VGRPresenter\logs\engine.log
timeout /t 4 >nul
