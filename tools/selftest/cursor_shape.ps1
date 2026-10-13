# tools/selftest/cursor_shape.ps1 — WHICH cursor is the OS actually showing?
#
# The app's own cursor machinery (AppCursor + the catcher in Main.qml) can be
# perfect in the model and still show nothing: what the user sees is the OS
# cursor, and only Win32 can answer that. GetCursorInfo returns the live cursor
# handle; comparing it against the standard cursors names it.
#
# Prints one line:  CURSOR: <NAME> h=<handle> at <x>,<y>
#   ARROW | IBEAM | HAND | SIZEALL | SIZENS | SIZEWE | SIZENWSE | SIZENESW |
#   CROSS | NO | WAIT | UPARROW | APPSTARTING | HELP | PIN | PERSON | unknown
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public int x; public int y; }
public class CursorProbe {
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO pci);
    [DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr hInstance, int lpCursorName);
    public static string Which() {
        CURSORINFO ci = new CURSORINFO();
        ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
        if (!GetCursorInfo(ref ci)) return "probe-failed";
        int[] ids = { 32512, 32513, 32514, 32515, 32516, 32631, 32640, 32641, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650, 32651, 32671 };
        string[] names = { "ARROW", "IBEAM", "WAIT", "CROSS", "UPARROW", "NO", "SIZEALL", "SIZENESW", "SIZENS", "SIZENWSE", "SIZEWE", "SIZEWAIT", "HAND", "APPSTARTING", "HELP", "PIN", "PERSON", "other-standard" };
        string name = "unknown";
        for (int i = 0; i < ids.Length; ++i) {
            if (LoadCursor(IntPtr.Zero, ids[i]) == ci.hCursor) { name = names[i]; break; }
        }
        return name + " h=" + ci.hCursor + " at " + ci.x + "," + ci.y;
    }
}
"@
Write-Output ("CURSOR: " + [CursorProbe]::Which())
