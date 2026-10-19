Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W2 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$p = Get-Process -Id 22224 -ErrorAction SilentlyContinue
if (-not $p) { Write-Output 'no process'; exit }
$h = $p.MainWindowHandle
[void][W2]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 600
function Snap($x, $y, $waitMs, $out) {
  [void][W2]::SetCursorPos($x, $y)
  Start-Sleep -Milliseconds $waitMs
  $bmp = New-Object System.Drawing.Bitmap 1504, 896
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 1504, 896))
  $g.Dispose()
  $bmp.Save("C:\Users\znwaj\Documents\dev_environment\Software\WindowsUI\tmp_diag\$out", [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output "saved $out at $x,$y"
}
Snap 40 700 2600 'idle2.png'
Snap 1477 330 800 'hover2.png'
