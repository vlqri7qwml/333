#Requires -Version 7.0
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$KitVersion,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'Visual Studio Installer / vswhere was not found.' }
$msbuildPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuildPath) { throw 'Install the Visual Studio 2022 C++ build tools.' }
$kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
if (-not $KitVersion) {
    $KitVersion = Get-ChildItem -LiteralPath (Join-Path $kitsRoot 'Include') -Directory |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'km\ntifs.h')) -and
            (Test-Path -LiteralPath (Join-Path $kitsRoot "Lib\$($_.Name)\km\x64\ntoskrnl.lib")) -and
            (Test-Path -LiteralPath (Join-Path $kitsRoot "Lib\$($_.Name)\um\x64\kernel32.lib"))
        } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
}
if (-not $KitVersion) { throw 'No matching x64 SDK + WDK headers/libraries were found.' }
Write-Host "Building x64 $Configuration with SDK/WDK $KitVersion"
# Materializing a fresh environment also collapses duplicate Path/PATH entries
# sometimes inherited from terminal hosts; .NET Framework MSBuild rejects them.
$buildStart = [System.Diagnostics.ProcessStartInfo]::new()
$buildStart.FileName = $msbuildPath
$buildStart.UseShellExecute = $false
$buildStart.CreateNoWindow = $true
$buildStart.RedirectStandardOutput = $true
$buildStart.RedirectStandardError = $true
$null = $buildStart.Environment.Count
@((Join-Path $PSScriptRoot 'Cracker.sln'), '/m', '/nodeReuse:false', '/nologo',
  '/verbosity:minimal', "/p:Configuration=$Configuration", '/p:Platform=x64',
  "/p:WindowsTargetPlatformVersion=$KitVersion") | ForEach-Object { $buildStart.ArgumentList.Add($_) }
$buildProcess = [System.Diagnostics.Process]::Start($buildStart)
$buildOutput = $buildProcess.StandardOutput.ReadToEndAsync()
$buildError = $buildProcess.StandardError.ReadToEndAsync()
$buildProcess.WaitForExit()
Write-Host $buildOutput.GetAwaiter().GetResult()
Write-Host $buildError.GetAwaiter().GetResult()
$buildExitCode = $buildProcess.ExitCode
$buildProcess.Dispose()
if ($buildExitCode -ne 0) { throw "Build failed: $buildExitCode" }
if (-not $SkipTests) {
    & (Join-Path $PSScriptRoot "out\x64\$Configuration\CrackerVerify.exe") --self-test
    if ($LASTEXITCODE -ne 0) { throw "Offline tests failed: $LASTEXITCODE" }
}
