param([Parameter(Mandatory=$true)][string]$CliPath)
$ErrorActionPreference = 'Stop'
$profileCli = [IO.Path]::GetFullPath($CliPath)
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporaryParent ('stfc-profiles-cli-' + [Guid]::NewGuid().ToString('N'))
$fixtureAbsolute = [IO.Path]::GetFullPath($fixtureRoot)
if (-not $fixtureAbsolute.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Synthetic CLI fixture escaped the temporary directory.'
}
New-Item -ItemType Directory -Path $fixtureAbsolute | Out-Null
function Invoke-ProfileCli {
    param([string[]]$CliArguments, [bool]$ExpectedSuccess=$true)
    $fullArguments = @('--root', $fixtureAbsolute, '--json') + $CliArguments
    # JSON is UTF-8 even when a hidden desktop PowerShell has an ANSI console
    # decoder. Do not change the caller's shared console code page for a test.
    $start = [Diagnostics.ProcessStartInfo]::new($profileCli)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false, $true)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false, $true)
    foreach ($argument in $fullArguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Could not start the CLI fixture process.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $resultText = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        $resultCode = $process.ExitCode
    } finally { $process.Dispose() }
    $result = $resultText | ConvertFrom-Json
    if ($ExpectedSuccess -and ($resultCode -ne 0 -or -not $result.ok)) {
        throw "CLI command failed: $($CliArguments -join ' '): $($resultText -join ' ')"
    }
    if (-not $ExpectedSuccess -and ($resultCode -eq 0 -or $result.ok)) {
        throw "CLI command unexpectedly succeeded: $($CliArguments -join ' ')"
    }
    return $result
}
try {
    $privateHelper = Invoke-ProfileCli -CliArguments @('--internal-user-import', '00000000000000000000000000000000', '0') -ExpectedSuccess $false
    if ($privateHelper.error.message -ne 'invalid private import helper arguments') {
        throw 'Private helper entry was rejected by the option parser before its strict validation.'
    }
    $missingUser = Invoke-ProfileCli -CliArguments @('import', 'Synthetic', '--user', 'NOT-A-WINDOWS-SID') -ExpectedSuccess $false
    if ($missingUser.error.code -ne 'source_user_missing') { throw 'Invalid import source did not return the shared source error.' }
    $emptyAfterFailedImport = Invoke-ProfileCli -CliArguments @('list')
    if ($emptyAfterFailedImport.profiles.Count -ne 0) { throw 'Failed source import published a profile.' }
    $created = Invoke-ProfileCli -CliArguments @('create', 'Science Ω')
    $profileId = $created.profile.id
    if ($profileId -notmatch '^[0-9a-f]{32}$') { throw 'CLI create did not return an immutable profile ID.' }
    $listed = Invoke-ProfileCli -CliArguments @('list')
    if ($listed.profiles.Count -ne 1 -or $listed.profiles[0].id -ne $profileId) { throw 'CLI list disagreed with shared catalog.' }
    $renamed = Invoke-ProfileCli -CliArguments @('rename', '--profile', $profileId, 'Engineering Ω')
    if ($renamed.profile.id -ne $profileId -or $renamed.profile.name -ne 'Engineering Ω') { throw 'CLI rename changed identity or lost Unicode.' }
    Invoke-ProfileCli -CliArguments @('rename', '--profile', $profileId, 'Stale', '--expected-revision', $created.profile.revision) -ExpectedSuccess $false | Out-Null
    $shortcutPath = Join-Path $fixtureAbsolute 'Science.lnk'
    Invoke-ProfileCli -CliArguments @('shortcut', '--profile', $profileId, '--output', $shortcutPath) | Out-Null
    if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) { throw 'Native shortcut was not created.' }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    if ($shortcut.TargetPath -ne $profileCli -or -not $shortcut.Arguments.Contains($profileId) -or $shortcut.Arguments.Contains('Engineering')) {
        throw 'Native shortcut was not anchored to the executable and immutable ID.'
    }
    Invoke-ProfileCli -CliArguments @('shortcut', '--profile', $profileId, '--output', $shortcutPath) -ExpectedSuccess $false | Out-Null
    $pathShortcut = Join-Path $fixtureAbsolute 'Path launch.lnk'
    $pathCommand = 'stfc-profiles --root "' + $fixtureAbsolute + '" --json shortcut --profile ' + $profileId + ' --output "' + $pathShortcut + '"'
    # This intentionally supplies a bare argv[0] from a different cwd, which
    # distinguishes process identity from how the shell found the executable.
    $previousProfileSearchPath = $env:PATH
    try {
        $env:PATH = (Split-Path -Parent $profileCli) + ';' + $env:PATH
        Push-Location -LiteralPath $fixtureAbsolute
        try {
            $pathResult = (& cmd.exe /d /s /c $pathCommand | Out-String) | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $pathResult.ok) { throw 'PATH-launched shortcut creation failed.' }
        } finally { Pop-Location }
    } finally { $env:PATH = $previousProfileSearchPath }
    if ($shell.CreateShortcut($pathShortcut).TargetPath -ne $profileCli) {
        throw 'PATH-launched shortcut did not resolve the actual executable.'
    }
    Invoke-ProfileCli -CliArguments @('delete', '--profile', $profileId) -ExpectedSuccess $false | Out-Null
    Invoke-ProfileCli -CliArguments @('game', 'status', '--profile', $profileId) -ExpectedSuccess $false | Out-Null
    Invoke-ProfileCli -CliArguments @('launch', '--profile', $profileId) -ExpectedSuccess $false | Out-Null
    Invoke-ProfileCli -CliArguments @('archive', '--profile', $profileId) | Out-Null
    $active = Invoke-ProfileCli -CliArguments @('list')
    $archived = Invoke-ProfileCli -CliArguments @('list', '--archived')
    if ($active.profiles.Count -ne 0 -or $archived.profiles.Count -ne 1 -or $archived.profiles[0].id -ne $profileId) {
        throw 'CLI archive did not preserve identity in the correct catalog bucket.'
    }
    Invoke-ProfileCli -CliArguments @('restore', '--profile', $profileId) | Out-Null
    Invoke-ProfileCli -CliArguments @('archive', '--profile', $profileId) | Out-Null
    Invoke-ProfileCli -CliArguments @('delete', '--profile', $profileId, '--archived', '--permanent') | Out-Null
    $empty = Invoke-ProfileCli -CliArguments @('list', '--archived')
    if ($empty.profiles.Count -ne 0) { throw 'Explicit permanent deletion retained an archived catalog entry.' }
    'profile CLI and native shortcut tests passed'
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureAbsolute)
    if (-not $resolvedFixture.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the verified temporary directory.'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
