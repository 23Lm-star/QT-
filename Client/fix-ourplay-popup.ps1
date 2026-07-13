$ErrorActionPreference = 'Continue'
$log = 'C:\tmp\fix-ourplay-popup.log'
Start-Transcript -Path $log -Force | Out-Null

$taskName = '\OurPlay电脑版-wcy_王晨扬'
Write-Host "Disabling scheduled task: $taskName"
& schtasks.exe /Change /TN $taskName /DISABLE
$disableExit = $LASTEXITCODE
Write-Host "schtasks exit code: $disableExit"

if ($disableExit -ne 0) {
    $path = 'C:\Program Files (x86)\ourplayer\op.runtimeconfig.json'
    $backup = 'C:\Program Files (x86)\ourplayer\op.runtimeconfig.json.bak-codex-20260708'
    Write-Host "Scheduled task disable failed; patching runtime config."

    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $path -Destination $backup
        Write-Host "Backed up runtime config to $backup"
    }

    $lines = @(
        '{'
        '  "runtimeOptions": {'
        '    "tfm": "net6.0",'
        '    "frameworks": ['
        '      {'
        '        "name": "Microsoft.NETCore.App",'
        '        "version": "6.0.13"'
        '      },'
        '      {'
        '        "name": "Microsoft.WindowsDesktop.App",'
        '        "version": "6.0.13"'
        '      }'
        '    ],'
        '    "configProperties": {'
        '      "System.Reflection.Metadata.MetadataUpdater.IsSupported": false'
        '    }'
        '  }'
        '}'
    )
    Set-Content -LiteralPath $path -Value $lines -Encoding UTF8
    Write-Host "Runtime config patched."
}

Stop-Transcript | Out-Null
