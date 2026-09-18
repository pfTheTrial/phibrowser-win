#!/bin/bash
# relaunch.sh [exe-on-box] [args]: kills any running setup, copies the fresh test
# installer over, starts it elevated through PhiSetupRun.
S=${PHI_SETUP_DIST:-$(dirname "$0")/../dist}
H=jdxin@192.168.100.170
EXE=${1:-C:\\Users\\jdxin\\setup\\PhiBrowserSetup-0.1.0.exe}
ARGS=${2:-}
ssh -o BatchMode=yes $H "Get-Process | Where-Object { \$_.ProcessName -like 'PhiBrowserSetup*' -or \$_.ProcessName -eq 'Uninstall' } | Stop-Process -Force -ErrorAction SilentlyContinue" 2>/dev/null
scp -q -o BatchMode=yes "$S/${3:-PhiBrowserSetup-0.1.0.exe}" $H:setup/
ssh -o BatchMode=yes $H "
Set-Content C:\Users\jdxin\setup\cmd.txt \"$EXE\"
if ('$ARGS' -ne '') { Add-Content C:\Users\jdxin\setup\cmd.txt '$ARGS' }
Remove-Item C:\Users\jdxin\setup\err.txt,C:\Users\jdxin\setup\pid.txt -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName PhiSetupRun
Start-Sleep 3
'pid ' + (Get-Content C:\Users\jdxin\setup\pid.txt -ErrorAction SilentlyContinue)
Get-Content C:\Users\jdxin\setup\err.txt -ErrorAction SilentlyContinue
" 2>&1 | tr -d '\r'
