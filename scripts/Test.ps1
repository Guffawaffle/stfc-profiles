param([string]$CommunityModRoot = '')

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$previousLocation = Get-Location
$records = [System.Collections.Generic.List[object]]::new()
$revision = 'unknown'
$dirtyState = ''
$hostRevision = $null
$hostDirtyState = $null
$passed = $false

function Invoke-RecordedXMake {
    param([string[]]$Arguments)
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $output = (& xmake @Arguments 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    $records.Add([ordered]@{
        command = 'xmake ' + ($Arguments -join ' ')
        cwd = (Get-Location).Path
        durationMs = $timer.ElapsedMilliseconds
        exitCode = $exitCode
        output = $output.Trim().Substring(0, [Math]::Min(8000, $output.Trim().Length))
    })
    Write-Host $output.Trim()
    if ($exitCode -ne 0) { throw "XMake failed with exit code $exitCode" }
}

try {
    Set-Location $taskRoot
    $revision = (& git rev-parse --verify HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { $revision = 'initial-uncommitted-extraction' }
    $dirtyState = (& git status --short | Out-String).Trim()
    if ($CommunityModRoot) {
        $hostRevision = (& git -C $CommunityModRoot rev-parse HEAD | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the explicit host revision' }
        $hostDirtyState = (& git -C $CommunityModRoot status --short | Out-String).Trim()
    }
    $configure = @('f', '-c', '-p', 'windows', '-a', 'x64', '-m', 'release', '-y')
    if ($CommunityModRoot) { $configure += "--community_mod_root=$CommunityModRoot" }
    Invoke-RecordedXMake -Arguments $configure
    Invoke-RecordedXMake -Arguments @('build', '-y')
    Invoke-RecordedXMake -Arguments @('run', 'legacy-contract-tests')
    Invoke-RecordedXMake -Arguments @('run', 'prefs-store-tests')
    Invoke-RecordedXMake -Arguments @('run', 'consumer-smoke')
    if ($CommunityModRoot) {
        Invoke-RecordedXMake -Arguments @('build', '-y', 'stfc-profiles-community-mod-adapter')
    }
    Set-Location (Join-Path $taskRoot 'examples/consumer')
    Invoke-RecordedXMake -Arguments @('f', '-P', '.', '-c', '-p', 'windows', '-a', 'x64', '-m', 'release', '-y')
    Invoke-RecordedXMake -Arguments @('build', '-P', '.', '-y')
    Invoke-RecordedXMake -Arguments @('run', '-P', '.', 'example-consumer')
    $passed = $true
} finally {
    $evidencePath = Join-Path $taskRoot 'artifacts/verification.json'
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $evidencePath))
    $receipt = [ordered]@{
        revision = $revision
        dirtyState = $dirtyState
        extractionSourceRevision = '323fb857f51f4cb08231d4b150ea8b5bb59340d1'
        timestampUtc = [DateTime]::UtcNow.ToString('o')
        passed = $passed
        communityModRoot = $CommunityModRoot
        communityModRevision = $hostRevision
        communityModDirtyState = $hostDirtyState
        communityModRevisionAfter = $(if ($CommunityModRoot) { (& git -C $CommunityModRoot rev-parse HEAD | Out-String).Trim() } else { $null })
        commands = $records.ToArray()
    }
    [System.IO.File]::WriteAllText($evidencePath, (($receipt | ConvertTo-Json -Depth 8) + "`n"), [System.Text.UTF8Encoding]::new($false))
    Set-Location $previousLocation
}
