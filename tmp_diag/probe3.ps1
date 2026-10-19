Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$p = Get-Process -Id 22224 -ErrorAction SilentlyContinue; if (-not $p) { 'no process'; exit }
[void][M]::SetForegroundWindow($p.MainWindowHandle)
[void][M]::SetCursorPos(700, 830)
Start-Sleep -Milliseconds 1500
$bmp = New-Object System.Drawing.Bitmap 1504, 896
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size 1504, 896))
$g.Dispose()
$bmp.Save('C:\Users\znwaj\Documents\dev_environment\Software\WindowsUI\tmp_diag\now.png', [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
'captured'
