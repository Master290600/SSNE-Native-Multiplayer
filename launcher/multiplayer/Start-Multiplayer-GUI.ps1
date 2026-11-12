param(
    [ValidateSet('Host','Client')][string]$Mode='Client',
    [string]$Address='', [string]$Pin='', [int]$Port=27341,
    [string]$Project='', [string]$PlayerName='Sam', [ValidateRange(0,10)][int]$PlayerSkin=0, [ValidateRange(1,5)][int]$EnemyMultiplier=1, [ValidateRange(0,5)][int]$AmmoCapacity=0, [ValidateRange(1,5)][int]$AmmoPickupMultiplier=1, [switch]$Describe, [switch]$ValidateOnly, [switch]$Fullscreen
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Text.UTF8Encoding]::new($false)
function File-Sha256([string]$Path) {
    $stream=[IO.File]::OpenRead($Path);$algorithm=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','') }
    finally {$stream.Dispose();$algorithm.Dispose()}
}
function Has-GameData([string]$Root) {
    return $Root -and [IO.File]::Exists((Join-Path $Root 'game_data/sys/boot.bin')) -and [IO.File]::Exists((Join-Path $Root 'game_data/sys/main.dol'))
}
$build=0;$protocol=0
try {
    $manifestPath=Join-Path $PSScriptRoot 'release-manifest.json'
    if(![IO.File]::Exists($manifestPath)){throw 'Der Multiplayer-Build fehlt. Bitte den Launcher im Multiplayer-Ordner starten.'}
    $manifest=[IO.File]::ReadAllText($manifestPath)|ConvertFrom-Json
    $build=[int]$manifest.build;$protocol=[int]$manifest.protocol
    if($build -lt 116){throw 'Der Launcher benötigt Multiplayer-Build 116 oder neuer.'}
    $file=[IO.Path]::GetFileName([string]$manifest.destination)
    if($file -notmatch '^(?:SSNE-Native-Multiplayer-Game|SSNE-RECOOP-Game(?:-v[0-9]+)?|SillyReEncounter-Entwicklung-v[0-9]+(?:-PC)?)\.exe$' -or $manifest.product_sha256 -notmatch '^[A-Fa-f0-9]{64}$'){throw 'Die Build-Beschreibung ist ungültig. Bitte den Multiplayer-Ordner erneut bereitstellen.'}
    $exe=Join-Path $PSScriptRoot $file
    if(![IO.File]::Exists($exe)){throw 'Die Multiplayer-Spieldatei fehlt neben dem Launcher.'}
    if((File-Sha256 $exe) -ne $manifest.product_sha256){throw 'Die Spieldatei passt nicht zum freigegebenen Build. Bitte den Multiplayer-Build erneut bereitstellen.'}
    if(!$Project){
        $candidates=@();$source=[string]$manifest.source
        if($source){$root=Split-Path -Parent $source;for($n=0;$n -lt 5 -and $root;$n++){$candidates+=$root;$root=Split-Path -Parent $root}}
        $root=$PSScriptRoot;for($n=0;$n -lt 5 -and $root;$n++){$candidates+=$root;$root=Split-Path -Parent $root}
        foreach($candidate in $candidates){if(Has-GameData $candidate){$Project=$candidate;break}}
    }
    if(!(Has-GameData $Project)){throw 'Bitte unter „Spieldaten“ den Spielordner mit dem Ordner game_data auswählen.'}
    $Project=[IO.Path]::GetFullPath($Project)
    $data=Join-Path $Project 'game_data';$disc=[IO.File]::ReadAllBytes((Join-Path $data 'sys/boot.bin'))
    if($disc.Length -lt 6 -or [Text.Encoding]::ASCII.GetString($disc,0,4) -ne 'G3BP'){throw 'Diese Spieldaten passen nicht zum Multiplayer-Build. Benötigt wird die europäische GameCube-Version.'}
    $region='EUR';$card=Join-Path $PSScriptRoot "saves/$Mode-MemoryCardA.$region.raw"
    $info=@{ok=$true;ready=$true;build=$build;protocol=$protocol;project=$Project;exe=$exe;mode=$Mode;message='Bereit';card=$card}
    if($Describe){$info|ConvertTo-Json -Compress;return}
    $Address=$Address.Trim();$Pin=$Pin.Trim();$PlayerName=$PlayerName.Trim()
    if(!$PlayerName -or $PlayerName.Length -gt 24 -or [Text.Encoding]::UTF8.GetByteCount($PlayerName) -gt 64 -or $PlayerName -match '[\x00-\x1f\x7f]'){throw 'Der Spielername muss 1 bis 24 Zeichen enthalten, ohne Steuerzeichen.'}
    if($Port -lt 1024 -or $Port -gt 65535){throw 'Der Port muss zwischen 1024 und 65535 liegen.'}
    if($Mode -eq 'Client'){
        $parsed=$null
        if($Address -notmatch '^[0-9]{1,3}(\.[0-9]{1,3}){3}$' -or ![Net.IPAddress]::TryParse($Address,[ref]$parsed) -or $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork){throw 'Bitte eine gültige Host-IP eingeben, zum Beispiel 192.168.178.20.'}
        if($Pin -notmatch '^[0-9]{6}$'){throw 'Bitte die sechsstellige PIN aus dem Spiel des Hosts eingeben.'}
        $Address=$parsed.ToString()
    }elseif($Pin -and $Pin -notmatch '^[0-9]{6}$'){throw 'Die eigene PIN muss genau sechs Ziffern enthalten oder leer bleiben.'}
    if($Mode -ne 'Host' -and ($EnemyMultiplier -ne 1 -or $AmmoCapacity -ne 0 -or $AmmoPickupMultiplier -ne 1)){throw 'Nur der Host kann die Spielregeln festlegen.'}
    $arguments=@('--data',$data,'--memcard',$card,'--lan-port',[string]$Port)
    $arguments+=@('--player-name',$PlayerName,'--player-skin',[string]$PlayerSkin)
    if($Mode -eq 'Host'){$arguments+=@('--coop-host','--coop-capacity','8','--enemy-multiplier',[string]$EnemyMultiplier,'--ammo-capacity',@('normal','double','unlimited','3','4','5')[$AmmoCapacity],'--ammo-pickup-multiplier',[string]$AmmoPickupMultiplier)}else{$arguments+=@('--coop-join',$Address)}
    if($Pin){$arguments+=@('--lan-pin',$Pin)}
    if($Fullscreen){$arguments+='--fullscreen'}
    if($ValidateOnly){$info.arguments=$arguments;$info.fullscreen=[bool]$Fullscreen;$info|ConvertTo-Json -Depth 4 -Compress;return}
    [IO.Directory]::CreateDirectory((Split-Path -Parent $card))|Out-Null
    if(![IO.File]::Exists($card)){$original=Join-Path $Project "saves/MemoryCardA.$region.raw";if([IO.File]::Exists($original)){[IO.File]::Copy($original,$card,$false)}}
    $settings=Join-Path $PSScriptRoot 'settings-netplay.ini'
    if(![IO.File]::Exists($settings)){
        [IO.File]::WriteAllText($settings,"[graphics]`nrenderer = gpu`nscale = 1`naspect = 1`nfps_limit = 60`nvsync = 0`nwindow_w = 960`nwindow_h = 540`nwindow_maximized = 0`nfullscreen = 0`n[audio]`nmuted = 0`n",[Text.UTF8Encoding]::new($false))
    }
    $logs=Join-Path $PSScriptRoot 'launcher-logs';[IO.Directory]::CreateDirectory($logs)|Out-Null
    $id=$Mode+'-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
    # Only this helper's child environment changes. User settings stay intact.
    foreach($entry in @([Environment]::GetEnvironmentVariables('Process').Keys)){if([string]$entry -like 'SSNE_*'){[Environment]::SetEnvironmentVariable([string]$entry,$null,'Process')}}
    $env:SSNE_PERF_LOG=Join-Path $logs "$id.performance.jsonl"
    $quoted=($arguments|ForEach-Object {'"'+$_.Replace('"','\"')+'"'}) -join ' '
    $process=Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $PSScriptRoot -WindowStyle Normal -RedirectStandardOutput "$logs/$id.stdout.log" -RedirectStandardError "$logs/$id.stderr.log" -PassThru
    try{$process.PriorityClass='BelowNormal'}catch{}
    if($process.WaitForExit(350) -and $process.ExitCode -ne 0){throw "Das Spiel konnte nicht starten (Fehler $($process.ExitCode)). Details stehen im Protokollordner."}
    $safeArgs=@($arguments);$pinIndex=[Array]::IndexOf($safeArgs,'--lan-pin');if($pinIndex -ge 0){$safeArgs[$pinIndex+1]='******'}
    $info.pid=$process.Id;$info.started=[DateTime]::Now.ToString('o');$info.message='Spiel gestartet.'
    @{started=$info.started;mode=$Mode;pid=$process.Id;sha256=$manifest.product_sha256;exe=$exe;arguments=$safeArgs}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath "$logs/$id.build.json" -Encoding UTF8
    $info|ConvertTo-Json -Depth 4 -Compress
}catch{
    @{ok=$false;ready=$false;build=$build;protocol=$protocol;message=$_.Exception.Message}|ConvertTo-Json -Compress
    if(!$Describe){exit 1}
}
