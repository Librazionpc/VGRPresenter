Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
'@
function Snap($x, $y, $waitMs, $out) {
  [void][W]::SetCursorPos($x, $y)
  Start-Sleep -Milliseconds $waitMs
  $p = Get-Process -Id 22224 -ErrorAction SilentlyContinue
  if (-not $p) { Write-Output 'no process'; exit }
  $h = $p.MainWindowHandle
  $w = 1504; $ht = 896
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][W]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save("C:\Users\znwaj\Documents\dev_environment\Software\WindowsUI\tmp_diag\$out", [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output "saved $out at cursor $x,$y"
}
Snap 40 700 2600 'idle.png'
Snap 1477 330 700 'hover.png'
