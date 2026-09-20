"""Give the machines back the way they were found.

Run this after the report. Two things the campaign changed and must undo:

  * the Interactive scheduled task the bench used to start the app in the
    console session (it could not be removed earlier without killing the app);
  * on the measuring station, the production MoonlightWeb that was uninstalled
    so the bench could have the machine to itself.

The DEV edition is deliberately LEFT INSTALLED everywhere: the PIN and the
rendezvous address in the report are only useful while it is there.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fleet  # noqa: E402
import install as installer  # noqa: E402


def drop_launch_tasks():
    for mid, m in fleet.MACHINES.items():
        if m["os"] != "windows":
            continue
        try:
            rc, out, _ = installer.cleanup_launch_task(mid)
            print("  %-10s %s" % (mid, (out or "").strip()[:60] or "rc=%s" % rc))
        except Exception as e:
            print("  %-10s could not clean up: %s" % (mid, e))


def reinstall_production(package):
    """Put the production edition back on the measuring station.

    Same silent flags as everywhere else. The logon task is not re-created by a
    silent install (it belongs to the Finished page), so it is registered here
    explicitly — the machine had one before the campaign took it away, and
    leaving without it would be leaving the machine worse than it was found.
    """
    if not os.path.exists(package):
        print("  production installer not found at %s" % package)
        return
    script = r"""
$pkg = '%s'
$p = Start-Process -FilePath $pkg -ArgumentList '%s' -PassThru
if (-not $p.WaitForExit(600000)) { $p.Kill(); Write-Output 'RESTORE: timed out'; exit 1 }
Write-Output ("RESTORE: installer exit " + $p.ExitCode)
Start-Sleep -Seconds 10

$exe = Join-Path $env:ProgramFiles 'MoonlightWeb\MoonlightWeb.exe'
if (-not (Test-Path $exe)) { Write-Output 'RESTORE: exe missing'; exit 1 }

# The logon task the Finished page would have created.
$t = Get-ScheduledTask -TaskName 'MoonlightWeb' -ErrorAction SilentlyContinue
if (-not $t) {
    try {
        $who = (Get-CimInstance Win32_ComputerSystem).UserName
        if (-not $who) { $who = "$env:USERDOMAIN\$env:USERNAME" }
        $act = New-ScheduledTaskAction -Execute $exe -Argument '--autostart'
        $trg = New-ScheduledTaskTrigger -AtLogOn -User $who
        $pr  = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName 'MoonlightWeb' -Action $act -Trigger $trg `
                               -Principal $pr -Force | Out-Null
        Write-Output 'RESTORE: logon task registered'
    } catch {
        Write-Output ('RESTORE: could not register the logon task: ' + ($_.Exception.Message -replace '"', ''))
    }
} else { Write-Output ('RESTORE: logon task already ' + $t.State) }

if (-not (Get-Process -Name 'MoonlightWeb' -ErrorAction SilentlyContinue)) {
    Start-ScheduledTask -TaskName 'MoonlightWeb'
    Start-Sleep -Seconds 15
}
$run = Get-Process -Name 'MoonlightWeb' -ErrorAction SilentlyContinue
Write-Output ('RESTORE: running ' + $(if ($run) { 'yes' } else { 'no' }))
""" % (package.replace("\\", "\\"), installer.WIN_SILENT)
    rc, out, err = fleet.run_script("local", script, timeout=900)
    print((out or "").strip()[-700:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prod-installer", default="",
                    help="path to the production installer to put back on this machine")
    ap.add_argument("--tasks-only", action="store_true")
    ns = ap.parse_args()

    print("removing the bench launch tasks…")
    drop_launch_tasks()
    if ns.prod_installer and not ns.tasks_only:
        print("reinstalling production on the measuring station…")
        reinstall_production(ns.prod_installer)


if __name__ == "__main__":
    main()
