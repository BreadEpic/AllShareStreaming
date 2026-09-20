# A window moved in a circle by the host itself — desktop motion with no input.
#
# The question it answers: when a stream stutters while the viewer drags a
# window round, is it the picture (sharp text sliding across the screen, heavy
# to encode) or the mouse (a report-rate flood of input messages, each one
# waking the capture)? Dragging by hand always brings both. This brings only
# the first: run it ON THE HOST, from the streamed desktop, then leave the
# client's mouse alone and read the [perf] lines.
#
#   powershell -NoProfile -File window-circle.ps1 -Seconds 30 -Hz 120 -Delay 5
#
# The window is filled with text on purpose: a flat rectangle compresses to
# nothing and would prove nothing.
param([int] $Seconds = 30, [int] $Hz = 120, [int] $Delay = 5, [int] $Radius = 300,
      [double] $TurnsPerSecond = 1.0)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CircleNative {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms);
  [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint ms);
}
"@
[void][CircleNative]::SetProcessDpiAwarenessContext([IntPtr](-4))

$form = New-Object System.Windows.Forms.Form
$form.Text = 'window-circle'
$form.StartPosition = 'Manual'
$form.Size = New-Object System.Drawing.Size(900, 600)
$form.TopMost = $true
$box = New-Object System.Windows.Forms.TextBox
$box.Multiline = $true
$box.Dock = 'Fill'
$box.Font = New-Object System.Drawing.Font('Consolas', 10)
$box.Text = ((1..60 | ForEach-Object { "line $_  the quick brown fox jumps over the lazy dog 0123456789 {}[]()<>;:" }) -join "`r`n")
$form.Controls.Add($box)

$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$cx = $screen.Left + ($screen.Width - $form.Width) / 2
$cy = $screen.Top + ($screen.Height - $form.Height) / 2
$form.Location = New-Object System.Drawing.Point([int]$cx, [int]$cy)
$form.Show()
[System.Windows.Forms.Application]::DoEvents()

Write-Host "Starting in $Delay s - leave the client's mouse alone."
Start-Sleep -Seconds $Delay

# SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
$flags = 0x0001 -bor 0x0004 -bor 0x0010
[void][CircleNative]::timeBeginPeriod(1)
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$period = 1000.0 / $Hz
$moves = 0
try {
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $angle = 2 * [Math]::PI * $TurnsPerSecond * $clock.Elapsed.TotalSeconds
        $x = [int]($cx + $Radius * [Math]::Cos($angle))
        $y = [int]($cy + $Radius * [Math]::Sin($angle))
        [void][CircleNative]::SetWindowPos($form.Handle, [IntPtr]::Zero, $x, $y, 0, 0, $flags)
        [System.Windows.Forms.Application]::DoEvents()
        $moves++
        # Sleep most of the period, spin the rest: Sleep alone is a millisecond
        # coarse even at timeBeginPeriod(1), and 120 Hz leaves 8.3 of them.
        $next = $moves * $period
        $left = $next - $clock.Elapsed.TotalMilliseconds
        if ($left -gt 2) { Start-Sleep -Milliseconds ([int]($left - 1.5)) }
        while ($clock.Elapsed.TotalMilliseconds -lt $next) { }
    }
} finally {
    [void][CircleNative]::timeEndPeriod(1)
    $form.Close()
}
$rate = [Math]::Round($moves / $clock.Elapsed.TotalSeconds, 1)
Write-Host "$moves moves in $([Math]::Round($clock.Elapsed.TotalSeconds, 1)) s ($rate/s)"
