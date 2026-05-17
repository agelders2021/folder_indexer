@echo off
echo Compiling sqlite3...
cl /c /W0 /O2 sqlite3.c /Fosqlite3.obj
if errorlevel 1 goto :err
echo Compiling resources...
rc /nologo file_indexer.rc
if errorlevel 1 goto :err
echo Compiling file_indexer...
cl /EHsc /W3 /O2 /std:c++17 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   file_indexer.cpp sqlite3.obj file_indexer.res ^
   /link user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib ^
   /subsystem:windows /out:file_indexer.exe
if errorlevel 1 goto :err
echo Build OK.
goto :done
:err
echo Build FAILED.
:done
