# Run the installed game twice, each with a private working directory and card.
$ErrorActionPreference = 'Stop'
$installation = Split-Path -Parent $PSScriptRoot
$game = Join-Path $installation 'SSNE-Native-Game.exe'
$data = Join-Path $installation 'game_data'
$testRoot = Join-Path $installation 'LAN-Test'
$launchLock = $null
$hostProcess = $null
$clientProcess = $null

function Start-TestGame($role, $arguments) {
    $directory = Join-Path $testRoot $role
    $shared = @('--user-dir', ('"{0}"' -f $directory), '--memcard', 'card.raw',
                '--lan-port', $port, '--lan-pin', $pin, '--player-name', $role)
    $process = Start-Process -FilePath $game -WorkingDirectory $directory -WindowStyle Normal `
        -ArgumentList ($shared + $arguments) -PassThru `
        -RedirectStandardOutput (Join-Path $directory 'console.log') `
        -RedirectStandardError (Join-Path $directory 'start-error.log')
    @($process.Id, $process.StartTime.ToFileTimeUtc()) | Set-Content -LiteralPath (Join-Path $directory 'process.txt') -Encoding ASCII
    return $process
}

function Wait-TestLog($role, $process, $message, $started) {
    $log = Join-Path $testRoot "$role/ssne-coop-$($role.ToLowerInvariant()).log"
    while ($true) {
        if ($process.HasExited) { throw "$role wurde beendet. Startprotokoll: $testRoot\$role\start-error.log" }
        if ((Test-Path -LiteralPath $log) -and [IO.File]::GetLastWriteTimeUtc($log) -ge $started -and
            (Select-String -LiteralPath $log -SimpleMatch $message -Quiet)) { return }
        if ([DateTime]::UtcNow -gt $started.AddMinutes(3)) { throw "$role ist noch nicht bereit. Protokoll: $log" }
        Start-Sleep -Milliseconds 250
    }
}

try {
    if (!(Test-Path -LiteralPath $game -PathType Leaf) -or !(Test-Path -LiteralPath (Join-Path $data 'sys/main.dol') -PathType Leaf)) {
        throw 'Spiel oder Spieldaten fehlen. Bitte zuerst im Launcher das Spiel bauen.'
    }
    [void][IO.Directory]::CreateDirectory($testRoot)
    try {
        $launchLock = [IO.File]::Open((Join-Path $testRoot 'start.lock'), 'OpenOrCreate', 'Write', 'None')
    } catch [IO.IOException] { throw 'Der lokale Test startet bereits.' }

    $card = @('Host-MemoryCardA.EUR.raw', 'MemoryCardA.EUR.raw') |
        ForEach-Object { Join-Path $installation "Multiplayer/saves/$_" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    Add-Type -AssemblyName System.Windows.Forms
    $area = [Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $width = [Math]::Min(960, [int]($area.Width / 2) - 18)
    $height = [Math]::Min([int]($width * 9 / 16), $area.Height - 64)
    foreach ($role in 'Host','Client') {
        $directory = Join-Path $testRoot $role
        $identity = Join-Path $directory 'process.txt'
        if (Test-Path -LiteralPath $identity -PathType Leaf) {
            $previous = @(Get-Content -LiteralPath $identity)
            if ($previous.Count -eq 2 -and $previous[0] -match '^\d+$') {
                $running = Get-Process -Id $previous[0] -ErrorAction SilentlyContinue
                if ($running -and $running.Path -eq $game -and $running.StartTime.ToFileTimeUtc().ToString() -eq $previous[1]) {
                    throw "Das Testfenster $role ist bereits geoeffnet. Bitte zuerst beide Testfenster schliessen."
                }
            }
        }
        [void][IO.Directory]::CreateDirectory($directory)
        $onlineIcon = Join-Path $installation 'Multiplayer/online-icon.png'
        if (Test-Path -LiteralPath $onlineIcon) { Copy-Item -LiteralPath $onlineIcon -Destination $directory -Force }
        $privateCard = Join-Path $directory 'card.raw'
        if ($card -and !(Test-Path -LiteralPath $privateCard)) {
            Copy-Item -LiteralPath $card -Destination $privateCard
        }
        $settings = Join-Path $directory 'settings-netplay.ini'
        if (!(Test-Path -LiteralPath $settings)) {
            $x = $area.Left + 8
            if ($role -eq 'Client') { $x += [int]($area.Width / 2) }
            $muted = [int]($role -eq 'Host')
            @('[graphics]', 'renderer=gpu', 'scale=1', 'fps_limit=60', 'fullscreen=0',
              'window_maximized=0', "window_x=$x", "window_y=$($area.Top + 36)", "window_w=$width", "window_h=$height",
              '[audio]', "muted=$muted", '[general]', 'skip_intros=1', 'pause_unfocused=0') |
                Set-Content -LiteralPath $settings -Encoding ASCII
        }
    }

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { $port = $listener.LocalEndpoint.Port } finally { $listener.Stop() }
    $pin = Get-Random -Minimum 100000 -Maximum 1000000
    $env:SSNE_COOP_LOOPBACK_ONLY = '1'
    $started = [DateTime]::UtcNow
    Write-Host 'Host startet links, Client rechts. Im Hostfenster den Spieltyp waehlen.'
    $hostProcess = Start-TestGame Host @('--coop-host', '--coop-capacity', '8')
    Wait-TestLog Host $hostProcess '[port] settings:' $started
    $started = [DateTime]::UtcNow
    $clientProcess = Start-TestGame Client @('--coop-join', '127.0.0.1')
    Wait-TestLog Client $clientProcess '[coop-engine] native level queued:' $started
    Write-Host "Der Client hat die Sitzung auf 127.0.0.1:$port geladen."
} catch {
    if ($clientProcess -and !$clientProcess.HasExited) { [void]$clientProcess.CloseMainWindow() }
    if ($hostProcess -and !$hostProcess.HasExited) { [void]$hostProcess.CloseMainWindow() }
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    if ($launchLock) { $launchLock.Dispose() }
}
