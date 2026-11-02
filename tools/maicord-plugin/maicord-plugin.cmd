@echo off
setlocal
python -B "%~dp0maicord_plugin.py" %*
exit /b %errorlevel%
