# PhiSetupRun (RunLevel Highest): starts the program named in
# C:\Users\jdxin\setup\cmd.txt (first line = exe, second = arguments) in the
# interactive session, elevated, so the installer's UAC manifest never
# prompts. Writes pid.txt, errors to err.txt.
try {
  $lines = @(Get-Content C:\Users\jdxin\setup\cmd.txt)
  $exe = $lines[0]
  $argstr = if ($lines.Count -gt 1) { $lines[1] } else { '' }
  if ($argstr) { $p = Start-Process -FilePath $exe -ArgumentList $argstr -PassThru }
  else { $p = Start-Process -FilePath $exe -PassThru }
  $p.Id | Out-File C:\Users\jdxin\setup\pid.txt
} catch {
  $_ | Out-String | Out-File C:\Users\jdxin\setup\err.txt
}
