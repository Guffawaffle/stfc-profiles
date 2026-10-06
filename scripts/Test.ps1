param(
    [string]$CommunityModRoot = '',
    [ValidateSet('windows','macosx')][string]$Platform = $(if ($IsWindows) { 'windows' } else { 'macosx' }),
    [ValidateSet('x64','x86_64','arm64')][string]$Architecture = $(if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } elseif ($IsWindows) { 'x64' } else { 'x86_64' })
)

$ErrorActionPreference = 'Stop'
if ($IsWindows -and -not $env:WINDIR) { $env:WINDIR = $env:SystemRoot }
if ($Platform -eq 'macosx' -and $Architecture -eq 'x64') { $Architecture = 'x86_64' }
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

function Invoke-RecordedCliTests {
    $cliBinary = Join-Path $taskRoot "build/$Platform/$Architecture/release/stfc-profiles.exe"
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $output = (& pwsh -NoLogo -NoProfile -File (Join-Path $taskRoot 'tests/cli_test.ps1') -CliPath $cliBinary 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    $records.Add([ordered]@{
        command = 'pwsh -NoLogo -NoProfile -File tests/cli_test.ps1 -CliPath ' + $cliBinary
        cwd = (Get-Location).Path
        durationMs = $timer.ElapsedMilliseconds
        exitCode = $exitCode
        output = $output.Trim().Substring(0, [Math]::Min(8000, $output.Trim().Length))
    })
    Write-Host $output.Trim()
    if ($exitCode -ne 0) { throw "CLI tests failed with exit code $exitCode" }
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
    $configure = @('f', '-c', '-p', $Platform, '-a', $Architecture, '-m', 'release', '-y')
    if ($CommunityModRoot) { $configure += "--community_mod_root=$CommunityModRoot" }
    Invoke-RecordedXMake -Arguments $configure
    Invoke-RecordedXMake -Arguments @('build', '-y')
    Invoke-RecordedXMake -Arguments @('run', 'identity-tests')
    Invoke-RecordedXMake -Arguments @('run', 'prefs-store-tests')
    Invoke-RecordedXMake -Arguments @('run', 'catalog-tests')
    if ($Platform -eq 'windows') {
        Invoke-RecordedXMake -Arguments @('run', 'installation-tests')
        Invoke-RecordedXMake -Arguments @('run', 'user-import-source-tests')
        Invoke-RecordedXMake -Arguments @('run', 'user-import-transfer-tests', (Join-Path $taskRoot "build/$Platform/$Architecture/release/stfc-profiles.exe"), (Join-Path $taskRoot "build/$Platform/$Architecture/release/stfc-profiles-native.dll"))
    }
    Invoke-RecordedXMake -Arguments @('run', 'consumer-smoke')
    if ($Platform -eq 'windows') { Invoke-RecordedCliTests }
    Invoke-RecordedXMake -Arguments @('build', '-y', 'stfc-profiles-community-mod-adapter')
    Invoke-RecordedXMake -Arguments @('build', '-y', 'stfc-profiles-runtime')
    if ($Platform -eq 'windows') {
        Invoke-RecordedXMake -Arguments @('build', '-y', 'runtime-loader-tests')
        Invoke-RecordedXMake -Arguments @('run', 'runtime-loader-tests', (Join-Path $taskRoot "build/$Platform/$Architecture/release/version.dll"))
    }
    Set-Location (Join-Path $taskRoot 'examples/consumer')
    Invoke-RecordedXMake -Arguments @('f', '-P', '.', '-c', '-p', $Platform, '-a', $Architecture, '-m', 'release', '-y')
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
        platform = $Platform
        architecture = $Architecture
        communityModRoot = $CommunityModRoot
        communityModRevision = $hostRevision
        communityModDirtyState = $hostDirtyState
        communityModRevisionAfter = $(if ($CommunityModRoot) { (& git -C $CommunityModRoot rev-parse HEAD | Out-String).Trim() } else { $null })
        commands = $records.ToArray()
    }
    [System.IO.File]::WriteAllText($evidencePath, (($receipt | ConvertTo-Json -Depth 8) + "`n"), [System.Text.UTF8Encoding]::new($false))
    Set-Location $previousLocation
}
