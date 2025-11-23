@echo off
rem Convenience build script for the UnrealMCP plugin in PCG_Forest_test
setlocal

set UE_BAT="D:\UE_5.7\Engine\Build\BatchFiles\Build.bat"
set UPROJECT="C:\Users\Viper\Documents\Unreal Projects\PCG_Forest_test\PCG_Forest_test.uproject"
set TARGET=PCG_Forest_testEditor
set PLATFORM=Win64
set CONFIG=Development

call %UE_BAT% %TARGET% %PLATFORM% %CONFIG% %UPROJECT% -WaitMutex -NoHotReload

endlocal
