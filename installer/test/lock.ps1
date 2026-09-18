# lock.ps1 <path> <seconds>: holds the file open with no sharing.
param($path, $seconds)
$f = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
Start-Sleep $seconds
$f.Close()
