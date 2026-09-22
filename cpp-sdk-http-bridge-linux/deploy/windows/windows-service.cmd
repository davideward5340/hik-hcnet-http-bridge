@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "OLD_CP="
for /f "tokens=2 delims=:" %%C in ('chcp') do set "OLD_CP=%%C"
chcp 936 >nul
if errorlevel 1 goto :encoding_failed

set "ACTION=%~1"
set "NO_PAUSE=0"
if /i "%~2"=="/nopause" set "NO_PAUSE=1"
if /i "%~2"=="/check" set "NO_PAUSE=1"
set "EXE=%~dp0hik-sdk-http-bridge.exe"
set "CONFIG=%~dp0config.json"
set "SC=%SystemRoot%\System32\sc.exe"
set "REG=%SystemRoot%\System32\reg.exe"
set "TASKS=%SystemRoot%\System32\schtasks.exe"
set "FIND=%SystemRoot%\System32\findstr.exe"
set "PING=%SystemRoot%\System32\ping.exe"
set "KEY=HKLM\SYSTEM\CurrentControlSet\Services\hikbridge"
set "CREATED=0"
set "CHANGED=0"
set "OLD_EXISTS=0"
set "OLD_RUNNING=0"
set "LEGACY_RUNNING=0"
set "OLD_IMAGE="
set "OLD_START="
set "RESULT=1"

if /i "%ACTION%"=="install" goto :validate
if /i "%ACTION%"=="uninstall" goto :admin
echo [错误] 未知操作。请运行安装.bat 或卸载.bat。
goto :finish

:validate
echo [信息] 正在校验程序和配置...
if not exist "%EXE%" goto :missing_exe
if not exist "%CONFIG%" goto :missing_config
"%EXE%" --validate-config --config "%CONFIG%"
if errorlevel 1 goto :validation_failed
if /i "%~2"=="/check" goto :check_success

:admin
if /i "%~2"=="/check" goto :invalid_check
"%SystemRoot%\System32\fltmc.exe" >nul 2>&1
if errorlevel 1 goto :not_admin
set "BACKUP=%~dp0installation-backups\%ACTION%-%RANDOM%-%RANDOM%"
if exist "%BACKUP%" goto :backup_failed
mkdir "%BACKUP%" >nul 2>&1
if errorlevel 1 goto :backup_failed
echo [信息] 注册备份目录：
echo        "%BACKUP%"
call :snapshot_task hikbridge
if errorlevel 1 goto :finish
call :snapshot_task HikSdkHttpBridge
if errorlevel 1 goto :finish
call :query_service hikbridge
if errorlevel 1 goto :finish
set "OLD_EXISTS=%EXISTS%"
if "%STATE%"=="4" set "OLD_RUNNING=1"
if "%OLD_EXISTS%"=="1" call :snapshot_service
if errorlevel 1 goto :finish
call :query_service HikSdkHttpBridge
if errorlevel 1 goto :finish
if "%STATE%"=="4" set "LEGACY_RUNNING=1"
if "%EXISTS%"=="1" "%REG%" export "HKLM\SYSTEM\CurrentControlSet\Services\HikSdkHttpBridge" "%BACKUP%\HikSdkHttpBridge.reg" /y >nul
if errorlevel 1 goto :backup_failed
if /i "%ACTION%"=="uninstall" goto :uninstall

echo [信息] 正在停止旧任务和服务...
call :pause_task hikbridge
if errorlevel 1 goto :rollback
call :pause_task HikSdkHttpBridge
if errorlevel 1 goto :rollback
call :stop_service HikSdkHttpBridge
if errorlevel 1 goto :rollback
call :stop_service hikbridge
if errorlevel 1 goto :rollback

echo [信息] 正在注册 Windows 原生服务 hikbridge...
call :configure_service
if errorlevel 1 goto :rollback
set "CHANGED=1"
"%SC%" start hikbridge
if errorlevel 1 goto :rollback
call :wait_running
if errorlevel 1 goto :rollback

rem RUNNING is reported by the bridge only after SDK initialization and listen.
echo [信息] 服务已完成初始化并建立 HTTP 监听，正在设置失败恢复...
"%SC%" failure hikbridge reset= 86400 actions= restart/5000/restart/30000/restart/60000
if errorlevel 1 goto :partial_success
"%SC%" failureflag hikbridge 1
if errorlevel 1 goto :partial_success
call :remove_task hikbridge
if errorlevel 1 goto :partial_success
call :remove_task HikSdkHttpBridge
if errorlevel 1 goto :partial_success
call :remove_service HikSdkHttpBridge
if errorlevel 1 goto :partial_success
echo [成功] 服务 hikbridge 已安装并启动，启动类型为自动启动。
echo        可在 services.msc 中查看；请勿移动或删除当前安装目录。
set "RESULT=0"
goto :finish

:uninstall
echo [信息] 正在卸载服务及旧计划任务...
set "REMOVE_FAILED=0"
call :remove_task hikbridge
if errorlevel 1 set "REMOVE_FAILED=1"
call :remove_task HikSdkHttpBridge
if errorlevel 1 set "REMOVE_FAILED=1"
call :remove_service hikbridge
if errorlevel 1 set "REMOVE_FAILED=1"
call :remove_service HikSdkHttpBridge
if errorlevel 1 set "REMOVE_FAILED=1"
if "%REMOVE_FAILED%"=="1" goto :uninstall_failed
echo [成功] 服务和旧计划任务已移除。配置、日志、缓存和注册备份均已保留。
set "RESULT=0"
goto :finish

:rollback
echo [错误] 安装未完成，正在尝试恢复原注册状态...
set "ROLLBACK_FAILED=0"
if "%CHANGED%"=="0" goto :restart_previous
call :stop_service hikbridge
if errorlevel 1 goto :rollback_failed
if "%CREATED%"=="1" goto :remove_new_service
call :restore_service
if errorlevel 1 goto :rollback_failed
goto :restart_previous
:remove_new_service
call :remove_service hikbridge
if errorlevel 1 goto :rollback_failed
:restart_previous
if "%OLD_RUNNING%"=="0" goto :restart_legacy
call :restart_service hikbridge
if errorlevel 1 set "ROLLBACK_FAILED=1"
:restart_legacy
if "%LEGACY_RUNNING%"=="0" goto :restore_tasks
call :restart_service HikSdkHttpBridge
if errorlevel 1 set "ROLLBACK_FAILED=1"
:restore_tasks
call :restore_task hikbridge
if errorlevel 1 set "ROLLBACK_FAILED=1"
call :restore_task HikSdkHttpBridge
if errorlevel 1 set "ROLLBACK_FAILED=1"
if "%ROLLBACK_FAILED%"=="1" goto :rollback_failed
echo [信息] 原服务启动状态及旧任务定义已恢复；已尝试启动旧任务。
echo        请检查配置、端口占用、应用日志及事件查看器后重试。
echo        旧 CMD 任务可能遗留子进程，请确认旧实例已退出；本脚本不会按进程名称批量强杀。
goto :finish
:rollback_failed
echo [错误] 原状态未能完全恢复，请依据注册备份检查服务和任务。
echo        为避免重复监听，请勿重复启动多个桥接实例。
goto :finish

:snapshot_service
"%REG%" export "%KEY%" "%BACKUP%\hikbridge.reg" /y >nul
if errorlevel 1 exit /b 1
"%REG%" query "%KEY%" /v ImagePath >"%BACKUP%\image.txt" 2>nul
if errorlevel 1 exit /b 1
for /f "usebackq tokens=2,*" %%A in ("%BACKUP%\image.txt") do if /i "%%A"=="REG_EXPAND_SZ" set "OLD_IMAGE=%%B"
if not defined OLD_IMAGE for /f "usebackq tokens=2,*" %%A in ("%BACKUP%\image.txt") do if /i "%%A"=="REG_SZ" set "OLD_IMAGE=%%B"
"%REG%" query "%KEY%" /v Start >"%BACKUP%\start.txt" 2>nul
if errorlevel 1 exit /b 1
for /f "usebackq tokens=2,*" %%A in ("%BACKUP%\start.txt") do if /i "%%A"=="REG_DWORD" set "OLD_START=%%B"
set "OLD_DELAYED=0x0"
"%REG%" query "%KEY%" /v DelayedAutoStart >"%BACKUP%\delayed.txt" 2>nul
for /f "usebackq tokens=2,*" %%A in ("%BACKUP%\delayed.txt") do if /i "%%A"=="REG_DWORD" set "OLD_DELAYED=%%B"
if not defined OLD_IMAGE exit /b 1
if "%OLD_START%"=="0x2" set "OLD_START=auto"
if "%OLD_START%"=="0x3" set "OLD_START=demand"
if "%OLD_START%"=="0x4" set "OLD_START=disabled"
if "%OLD_START%"=="auto" if "%OLD_DELAYED%"=="0x1" set "OLD_START=delayed-auto"
if "%OLD_START%"=="auto" exit /b 0
if "%OLD_START%"=="delayed-auto" exit /b 0
if "%OLD_START%"=="demand" exit /b 0
if "%OLD_START%"=="disabled" exit /b 0
echo [错误] 无法读取原服务的路径或启动类型，已中止操作。
exit /b 1

:configure_service
rem Delayed expansion is enabled only AFTER paths have been captured. Expanded
rem values are not parsed as CMD operators; spaces, &, %, ! remain path data.
setlocal EnableDelayedExpansion
if "!OLD_EXISTS!"=="1" goto :update_existing
"!SC!" create hikbridge binPath= "\"!EXE!\" service --config \"!CONFIG!\"" start= auto obj= LocalSystem DisplayName= "Hik SDK HTTP Bridge"
set "RC=!errorlevel!"
endlocal & set "CREATED=1" & exit /b %RC%
:update_existing
"!SC!" config hikbridge binPath= "\"!EXE!\" service --config \"!CONFIG!\"" start= auto
set "RC=!errorlevel!"
endlocal & exit /b %RC%

:restore_service
setlocal EnableDelayedExpansion
set "ESCAPED=!OLD_IMAGE:"=\"!"
"!SC!" config hikbridge binPath= "!ESCAPED!" start= !OLD_START!
set "RC=!errorlevel!"
endlocal & exit /b %RC%

:query_service
set "EXISTS=0"
set "STATE="
"%SC%" query "%~1" >"%BACKUP%\service-query.txt" 2>&1
set "QUERY_RC=%errorlevel%"
if "%QUERY_RC%"=="1060" exit /b 0
if not "%QUERY_RC%"=="0" goto :query_failed
set "EXISTS=1"
"%FIND%" /R /C:"STATE *:" "%BACKUP%\service-query.txt" >"%BACKUP%\state.txt"
for /f "usebackq tokens=3" %%S in ("%BACKUP%\state.txt") do set "STATE=%%S"
if defined STATE exit /b 0
:query_failed
type "%BACKUP%\service-query.txt"
echo [错误] 无法确定服务状态，已中止当前操作。
exit /b 1

:stop_service
set "ITEM=%~1"
call :query_service %ITEM%
if errorlevel 1 exit /b 1
if "%EXISTS%"=="0" exit /b 0
if "%STATE%"=="1" exit /b 0
if "%STATE%"=="3" goto :wait_stopped
"%SC%" stop "%ITEM%"
if errorlevel 1 exit /b 1
:wait_stopped
set "WAIT_COUNT=0"
:stop_poll
call :query_service %ITEM%
if errorlevel 1 exit /b 1
if "%EXISTS%"=="0" exit /b 0
if "%STATE%"=="1" exit /b 0
set /a WAIT_COUNT+=1 >nul
if %WAIT_COUNT% GEQ 90 goto :stop_timeout
call :delay
goto :stop_poll
:stop_timeout
echo [错误] 等待服务停止超时，服务注册已保留。
exit /b 1

:wait_running
set "WAIT_COUNT=0"
:start_poll
call :query_service hikbridge
if errorlevel 1 exit /b 1
if "%EXISTS%"=="0" exit /b 1
if "%STATE%"=="1" exit /b 1
if "%STATE%"=="4" goto :confirm_running
set /a WAIT_COUNT+=1 >nul
if %WAIT_COUNT% GEQ 240 exit /b 1
call :delay
goto :start_poll
:confirm_running
call :delay
call :query_service hikbridge
if errorlevel 1 exit /b 1
if "%STATE%"=="4" exit /b 0
exit /b 1

:restart_service
call :query_service %~1
if errorlevel 1 exit /b 1
if "%STATE%"=="4" exit /b 0
"%SC%" start "%~1"
exit /b %errorlevel%

:remove_service
set "ITEM=%~1"
call :stop_service %ITEM%
if errorlevel 1 exit /b 1
call :query_service %ITEM%
if errorlevel 1 exit /b 1
if "%EXISTS%"=="0" exit /b 0
"%SC%" delete "%ITEM%"
if errorlevel 1 exit /b 1
set "WAIT_COUNT=0"
:delete_poll
call :query_service %ITEM%
if errorlevel 1 exit /b 1
if "%EXISTS%"=="0" exit /b 0
set /a WAIT_COUNT+=1 >nul
if %WAIT_COUNT% GEQ 30 goto :delete_timeout
call :delay
goto :delete_poll
:delete_timeout
echo [错误] 服务仍处于待删除状态，请关闭服务管理窗口，必要时重启后重试。
exit /b 1

:snapshot_task
"%TASKS%" /Query /TN "%~1" >nul 2>&1
if errorlevel 1 goto :task_not_found
"%TASKS%" /Query /TN "%~1" /XML >"%BACKUP%\%~1.xml"
exit /b %errorlevel%
:task_not_found
if exist "%SystemRoot%\System32\Tasks\%~1" goto :task_query_failed
exit /b 0
:task_query_failed
echo [错误] 无法查询已存在的计划任务，请检查任务计划程序服务。
exit /b 1

:pause_task
if not exist "%BACKUP%\%~1.xml" exit /b 0
"%TASKS%" /Change /TN "%~1" /DISABLE
if errorlevel 1 exit /b 1
"%TASKS%" /End /TN "%~1" >nul 2>&1
exit /b 0

:remove_task
if not exist "%BACKUP%\%~1.xml" exit /b 0
call :pause_task %~1
if errorlevel 1 exit /b 1
"%TASKS%" /Delete /TN "%~1" /F
exit /b %errorlevel%

:restore_task
if not exist "%BACKUP%\%~1.xml" exit /b 0
"%TASKS%" /Create /TN "%~1" /XML "%BACKUP%\%~1.xml" /F
if errorlevel 1 exit /b 1
"%TASKS%" /Run /TN "%~1" >nul 2>&1
rem A restored disabled task correctly refuses to run; its XML is preserved.
exit /b 0

:delay
"%PING%" -n 2 127.0.0.1 >nul 2>&1
exit /b 0

:check_success
echo [成功] 程序和配置校验通过；未修改任何服务或计划任务。
set "RESULT=0"
goto :finish
:invalid_check
echo [错误] /check 参数仅适用于安装批处理，不执行卸载。
goto :finish
:missing_exe
echo [错误] 找不到程序：
echo        "%EXE%"
goto :finish
:missing_config
echo [错误] 找不到配置文件：
echo        "%CONFIG%"
goto :finish
:validation_failed
echo [错误] 配置校验失败，未修改服务注册。
goto :finish
:not_admin
echo [错误] 需要管理员权限，请右键批处理文件，选择“以管理员身份运行”。
goto :finish
:backup_failed
echo [错误] 创建注册备份失败，请检查目录权限和磁盘空间。
goto :finish
:partial_success
echo [错误] 新服务已启动，但失败恢复设置或旧注册清理未完成。
echo        已保留可用的新服务，请检查上方错误后重新执行安装。
goto :finish
:uninstall_failed
echo [错误] 部分服务或任务未能移除，请检查上方错误后重试。
goto :finish
:encoding_failed
echo [ERROR] Cannot activate code page 936. No registration was changed.
set "RESULT=1"
set "NO_PAUSE=1"
:finish
echo.
if not "%NO_PAUSE%"=="1" pause
if defined OLD_CP chcp %OLD_CP% >nul
exit /b %RESULT%
