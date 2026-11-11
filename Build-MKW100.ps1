[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallRoot,
    [int]$Jobs = 6,
    [switch]$PreflightOnly,
    [switch]$AllowSourceDependencies
)
# Standalone, isolated Windows fork build. Never calls LocalBuild.ps1, never
# modifies original Install\Base or the older R13/R23 build cache.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = $Here
$Logs = Join-Path $Here 'build-logs'
$Toolkit = Join-Path $InstallRoot 'Toolkit'
$OriginalWorkspace = Join-Path $InstallRoot 'BuildWorkspace'
$NativeBuild = Join-Path $Repo 'native-build-win'
$BuildMarker = Join-Path $Logs 'build_success.txt'
$BuildLog = Join-Path $Logs 'build_steps.txt'
$RunStartedAt = Get-Date
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
function Record([string]$Text) {
    $line = '[mkw100] ' + $Text
    Write-Host $line
    Add-Content -LiteralPath $BuildLog -Value $line -Encoding UTF8
}
function Require([string]$File, [string]$Description) {
    if (-not (Test-Path -LiteralPath $File)) { throw ('Missing ' + $Description + ': ' + $File) }
}
function InvokeTool([string]$Executable, [string[]]$Arguments, [string]$Name) {
    Record ('Starting ' + $Name)
    $stdout = Join-Path $Logs ($Name + '.stdout.txt')
    $stderr = Join-Path $Logs ($Name + '.stderr.txt')
    # Windows PowerShell 5.1 compatible: Start-Process returns the process
    # object; WaitForExit waits only for THIS process, not its entire process
    # tree. Unlike native direct invocation, the process ExitCode is explicit.
    # Keep original Windows argument quoting (verified in the prior build).
    $quoted = @($Arguments | ForEach-Object {
        if ($_.Contains('"')) { throw ('Unsupported quote in native argument: ' + $_) }
        '"' + ($_ -replace '(\\+)$', '$1$1') + '"'
    })
    $child = Start-Process -FilePath $Executable -ArgumentList $quoted `
        -NoNewWindow -PassThru -WorkingDirectory $Repo `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ErrorAction Stop
    try {
        $handle = $child.Handle
        $child.WaitForExit()
        $code = [int]$child.ExitCode
    } finally {
        $child.Dispose()
    }
    $tail = @(Get-Content -LiteralPath $stdout -Tail 12 -ErrorAction SilentlyContinue)
    if ($code -ne 0) { $tail += @(Get-Content -LiteralPath $stderr -Tail 35 -ErrorAction SilentlyContinue) }
    foreach ($line in $tail) { Write-Host $line }
    if ($code -ne 0) { throw ($Name + ' failed with exit ' + $code + ' (see ' + $Logs + ')') }
    Record ('Completed ' + $Name)
}
function MakeJunction([string]$Target, [string]$Link) {
    Require $Target 'shared dependency directory'
    if (Test-Path -LiteralPath $Link) {
        $item = Get-Item -LiteralPath $Link -Force
        if ($item.LinkType -eq 'Junction') {
            $got = @($item.Target)[0]
            if ([IO.Path]::GetFullPath($got).TrimEnd('\') -ine [IO.Path]::GetFullPath($Target).TrimEnd('\')) {
                throw ('Junction points to unexpected directory: ' + $Link + ' -> ' + $got)
            }
            return
        }
        if (($item -is [IO.DirectoryInfo]) -and @([IO.Directory]::EnumerateFileSystemEntries($Link)).Count -eq 0) {
            Remove-Item -LiteralPath $Link -Force
        } else { throw ('Refusing to replace nonempty directory: ' + $Link) }
    }
    New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null
}
function PackageBuildEvidence { Write-Host ("Build logs: " + $Logs) }
try {
    ('MKW100 BUILD ATTEMPT: ' + (Get-Date -Format o)) | Set-Content -LiteralPath $BuildLog -Encoding UTF8
    if (Test-Path $BuildMarker) { Remove-Item $BuildMarker -Force }
    if (Test-Path (Join-Path $Logs 'build_failure.txt')) { Remove-Item (Join-Path $Logs 'build_failure.txt') -Force }
    Require (Join-Path $OriginalWorkspace 'aurora-main\CMakeLists.txt') 'original Aurora source'
    Require (Join-Path $OriginalWorkspace 'Assets\main.dol') 'user-owned main.dol'
    Require (Join-Path $OriginalWorkspace 'Assets\StaticR.rel') 'user-owned StaticR.rel'
    Require (Join-Path $OriginalWorkspace 'Dependencies') 'original cached dependencies'
    foreach ($f in @('CMake\bin\cmake.exe','Ninja\ninja.exe','llvm-mingw\bin\x86_64-w64-mingw32-clang.exe','llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe','llvm-mingw\bin\x86_64-w64-mingw32-windres.exe')) {
        Require (Join-Path $Toolkit $f) 'bundled native toolchain'
    }
    $dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
    if (-not $dotnet) { throw 'dotnet.exe not on PATH: fork needs a .NET SDK to compile its modified translator (the old bundled Translator.Cli.exe is incompatible).' }
    $sdkList = & $dotnet.Source --list-sdks
    if (-not $sdkList) { throw 'dotnet is present but no SDK installed. Install a .NET 8+ SDK, then rerun.' }
    Record ('dotnet SDKs: ' + (($sdkList | Select-Object -Last 3) -join '; '))
    Require (Join-Path $Repo 'projects\mkwii\mkw100\player-slot-sites.txt') 'mkw100 translation rewrite table'
    Require (Join-Path $Repo 'runtime\src\hle\boot_race.cpp') 'mkw100 race harness'
    $assets = Join-Path $Repo 'Assets'
    New-Item -ItemType Directory -Path $assets -Force | Out-Null
    $dol = Join-Path $assets 'main.dol'
    $rel = Join-Path $assets 'StaticR.rel'
    if (-not (Test-Path $dol)) { Copy-Item -LiteralPath (Join-Path $OriginalWorkspace 'Assets\main.dol') -Destination $dol }
    if (-not (Test-Path $rel)) { Copy-Item -LiteralPath (Join-Path $OriginalWorkspace 'Assets\StaticR.rel') -Destination $rel }
    $expectedDol = '80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05'
    $expectedRel = '16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d'
    if ((Get-FileHash $dol -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedDol -or
        (Get-FileHash $rel -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedRel) {
        throw 'Clean PAL RMCP01 DOL/REL checksum mismatch; refusing translation.'
    }
    MakeJunction (Join-Path $OriginalWorkspace 'aurora-main') (Join-Path $Repo 'aurora-main')
    MakeJunction (Join-Path $OriginalWorkspace 'Dependencies') (Join-Path $Repo 'Dependencies')
    Record 'Clean PAL inputs validated; shared Aurora/Dependencies are junctions (not copied).'
    Record ('Incremental fork cache: ' + $NativeBuild)
    if ($PreflightOnly) {
        Record 'PREFLIGHT PASSED — no translator, CMake or native compilation run.'
        PackageBuildEvidence
        exit 0
    }
    $bin = Join-Path $Toolkit 'llvm-mingw\bin'
    . (Join-Path $Repo 'Launcher\NativeBuildFlags.ps1')
    $prebuilt = Resolve-MkwNativePrebuiltDirectory (Join-Path $Repo 'Dependencies') $bin `
        (Join-Path $Repo 'aurora-main') (Join-Path $Repo 'runtime\third_party')
    if (-not $prebuilt -and -not $AllowSourceDependencies) {
        throw 'Existing native_prebuilt does not match the fork source/toolchain. Refusing surprise multi-GB source dependency build; rerun with -AllowSourceDependencies only if disk space permits.'
    }
    Record ('Native prebuilt: ' + $(if($prebuilt){$prebuilt}else{'unavailable; opted into source compilation'}))
    $cmdPath = Join-Path $env:SystemRoot 'System32\cmd.exe'
    Require $cmdPath 'Windows cmd.exe for process-exitcode smoke test'
    $translatorOut = Join-Path $Repo 'build-translator-win'
    InvokeTool $dotnet.Source @('build',(Join-Path $Repo 'translator\src\Translator.Cli\Translator.Cli.csproj'),'-c','Release','-o',$translatorOut,'-v','minimal','-nologo') '01_translator'
    $translatorDll = Join-Path $translatorOut 'Translator.Cli.dll'
    Require $translatorDll 'new fork translator CLI'
    Record 'Translator targets .NET 8; Major roll-forward allows the installed .NET 10 runtime.'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'--help') '01b_translator_hostcheck'
    $project = Join-Path $Repo 'projects\mkwii\recomp.yml'
    $metadata = Join-Path $Repo 'generated\base_translation_output.json'
    $functions = Join-Path $Repo 'generated\functions'
    $shards = Join-Path $Repo 'generated\build_shards'
    $baseMan = Join-Path $Repo 'build\base'
    $generated = Join-Path $Repo 'generated'
    New-Item -ItemType Directory -Path $generated,$baseMan -Force | Out-Null
    $threads = [Math]::Max(1,[Math]::Min(16,$Jobs))
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'translate-recursive','0x800060A4','--project',$project,
        '--outdir',$functions,'--output-metadata',$metadata,'--production-source-bundle',
        (Join-Path $generated 'base_translation_sources.bin'),'--no-function-files','--prune-stale',
        '--threads',"$threads") '02_translate'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'emit-base-manifest','--project',$project,'--out',$baseMan,
        '--functions-dir',$functions,'--translation-output-metadata',$metadata,'--region','P') '03_manifest'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'generate-data-init','--project',$project) '04_data_init'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'emit-build-shards','--project',$project,'--base-metadata',$metadata,
        '--base-functions-dir',$functions,'--native-source-dir',(Join-Path $Repo 'runtime\src'),'--out',$shards) '05_shards'
    $cmake = Join-Path $Toolkit 'CMake\bin\cmake.exe'
    $configure = Get-MkwNativeConfigureArguments `
        -SourceDirectory (Join-Path $Repo 'runtime') -BuildDirectory $NativeBuild `
        -Ninja (Join-Path $Toolkit 'Ninja\ninja.exe') `
        -CCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') `
        -CxxCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe') `
        -ResourceCompiler (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') `
        -DependenciesDirectory (Join-Path $Repo 'Dependencies') -NativePrebuiltDirectory $prebuilt `
        -AdditionalArguments @('-DMKW_TRANSLATED_COMPILE_JOBS=4','-DMKW_X86_CPU_PROFILE=v3')
    InvokeTool $cmake $configure '06_configure'
    InvokeTool $cmake @('--build',$NativeBuild,'--target','WiiCompiled','--parallel',"$threads") '07_compile'
    $builtExe = Join-Path $NativeBuild 'WiiCompiled.exe'
    Require $builtExe 'new mkw100 Windows executable'
    $sha = (Get-FileHash -LiteralPath $builtExe -Algorithm SHA256).Hash.ToLowerInvariant()
    @('BUILD_SUCCESS',('Executable: '+$builtExe),('SHA256: '+$sha),'Build cache is retained for all future changes') |
        Set-Content -LiteralPath $BuildMarker -Encoding UTF8
    Record ('BUILD SUCCESS. New executable SHA256 ' + $sha)
    PackageBuildEvidence
} catch {
    $errorText = $_.Exception.Message
    Write-Host ('[mkw100] BUILD/PREFLIGHT FAILED: ' + $errorText) -ForegroundColor Red
    ('BUILD_FAILED: '+$errorText) | Set-Content -LiteralPath (Join-Path $Logs 'build_failure.txt') -Encoding UTF8
    PackageBuildEvidence
    exit 1
}
