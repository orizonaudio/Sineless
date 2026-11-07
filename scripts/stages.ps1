# The Windows check's stages (dot-sourced by scripts\check.ps1), as scripts/stages.sh for the
# others: every stage runs, and the failures are listed at the end, so one run shows every finding.
# A stage that needs another runs only if that one passed. Nothing here is Windows-only, so it is
# tested under PowerShell on Linux (scripts/tests/stages-ps1.test.ts).
#
#   Stage -Name <name> [-Needs <names>] [-Rerun <command>] -Body { ... }
#   Report-Stages     the summary; exits 1 if any stage failed
#
# A body fails on a terminating error (Run's throw, or any error under
# $ErrorActionPreference = 'Stop'), which ends the body there; Ctrl+C still stops the whole run,
# since it is not an error a catch receives (about_Try_Catch_Finally).

$script:Stages = [System.Collections.Generic.List[object]]::new()

function Stage {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [string[]] $Needs = @(),
        [string] $Rerun = '',
        [Parameter(Mandatory)] [scriptblock] $Body
    )
    Write-Host "`n==> $Name"
    foreach ($need in $Needs) {
        $before = $script:Stages | Where-Object { $_.Name -eq $need }
        # A typo in a stage's needs would otherwise skip it silently.
        if (-not $before) { throw "Stage $Name needs '$need', which is not a stage before it." }
        if ($before.Result -ne 'passed') {
            Write-Host "Skipped: '$need' did not pass."
            $script:Stages.Add([pscustomobject]@{ Name = $Name; Result = 'skipped'; Note = "needs $need"; Seconds = $null; Rerun = '' })
            return
        }
    }
    $start = Get-Date
    try {
        & $Body
        $result = 'passed'
        $note = ''
    } catch {
        Write-Host "Failed: $_"
        $result = 'failed'
        $note = 'error'
    }
    $seconds = [int]((Get-Date) - $start).TotalSeconds
    $script:Stages.Add([pscustomobject]@{ Name = $Name; Result = $result; Note = $note; Seconds = $seconds; Rerun = $Rerun })
}

function Report-Stages {
    Write-Host "`n==> Summary"
    $failed = 0
    foreach ($stage in $script:Stages) {
        $minutes = if ($null -ne $stage.Seconds) { '{0}.{1} min' -f [math]::Floor($stage.Seconds / 60), [math]::Floor($stage.Seconds % 60 / 6) } else { '' }
        $label = if ($stage.Result -eq 'failed') { $failed++; 'FAILED' } else { $stage.Result }
        $note = if ($stage.Note) { " ($($stage.Note))" } else { '' }
        $shown = if ($stage.Result -eq 'skipped') { '' } else { $minutes }
        Write-Host ('  {0,-8} {1,9}  {2}{3}' -f $label, $shown, $stage.Name, $note)
    }
    $reruns = @($script:Stages | Where-Object { $_.Result -eq 'failed' -and $_.Rerun })
    if ($reruns.Count) {
        Write-Host 'Rerun a failed stage alone:'
        foreach ($stage in $reruns) { Write-Host "  $($stage.Name): $($stage.Rerun)" }
    }
    if ($failed) {
        Write-Host "`n$failed of $($script:Stages.Count) stages failed."
        exit 1
    }
    Write-Host "`nAll checks passed."
}
