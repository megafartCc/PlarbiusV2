param(
    [string]$ConfigName = "bucket_superhuman_32gb",
    [int]$Hours = 20,
    [int]$Workers = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"

$BuildDir = ".\build\msvc-release\Release"
$DataDir = ".\data\$ConfigName"
$ConfigPath = ".\configs\hunl\$ConfigName.cfg"
$ActionConfigPath = ".\configs\hunl\action_32gb.cfg"

if (-Not (Test-Path $ConfigPath)) {
    Write-Host "Error: Config file not found at $ConfigPath" -ForegroundColor Red
    exit 1
}

# 1. Build the Abstraction Tables (if they don't already exist)
$BinFile = "$DataDir\${ConfigName}.bin"
if (-Not (Test-Path $BinFile)) {
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "Building Abstraction Tables for $ConfigName..." -ForegroundColor Cyan
    Write-Host "This will take several hours." -ForegroundColor Yellow
    Write-Host "==================================================" -ForegroundColor Cyan
    
    if (-Not (Test-Path $DataDir)) {
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    }

    $SanityExe = "$BuildDir\plarbius_bucket_sanity.exe"
    if (-Not (Test-Path $SanityExe)) {
        Write-Host "Error: Cannot find $SanityExe. Did you build the Release profile?" -ForegroundColor Red
        exit 1
    }

    $BuildStart = Get-Date

    # plarbius_bucket_sanity generates the .bin lookup table in the same directory as the config, 
    # but the game engine expects it. We will just run it. The engine actually loads it via LoadHunlActionAbstractionConfig etc.
    # WAIT, actually plarbius_train_hunl builds it in memory if it's missing, but it takes forever.
    # The standard way in PlarbiusV2 is just letting train_hunl_main do it on boot.
    # However, to be safe and separate the clustering step from training:
    
    $ArgsSanity = @(
        "--bucket-config", $ConfigPath,
        "--output", $BinFile
    )
    
    # In PlarbiusV2, the bucket lookup doesn't write out natively from sanity. 
    # Actually train_hunl_main builds the table on the fly and doesn't serialize it to disk by default.
    # It just holds it in RAM. So we don't need a separate build step, we just start the server!
}

# 2. Start IPC Training
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "Starting Distributed IPC Training" -ForegroundColor Cyan
Write-Host "Config: $ConfigName" -ForegroundColor Cyan
Write-Host "Workers: $Workers" -ForegroundColor Cyan
Write-Host "Duration: $Hours Hours" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

$TrainExe = "$BuildDir\plarbius_train_hunl.exe"
if (-Not (Test-Path $TrainExe)) {
    Write-Host "Error: Cannot find $TrainExe. Did you build the Release profile?" -ForegroundColor Red
    exit 1
}

$ServerName = "plarbius_hunl_$ConfigName"
$MetricsPath = "$DataDir\metrics.csv"
$PolicyPath = "$DataDir\policy.bin"
$CheckpointPath = "$DataDir\checkpoint.bin"

# Clean up stale shutdown signal from previous crashed runs
if (Test-Path ".\.stop_ipc_server") {
    Write-Host "Removing stale .stop_ipc_server file from previous run..." -ForegroundColor Yellow
    Remove-Item ".\.stop_ipc_server" -Force
}

# 2a. Start the Master Server
Write-Host "Starting IPC Server..."
$ServerArgs = @(
    "1000000000", # 1 Billion iterations (will be cut off by time)
    "--bucket-config", $ConfigPath,
    "--action-config", $ActionConfigPath,
    "--metrics-out", $MetricsPath,
    "--metrics-interval", "100000",
    "--policy-out", $PolicyPath,
    "--checkpoint", $CheckpointPath,
    "--checkpoint-every", "1000000",
    "--ipc-server", $ServerName
)

# If a checkpoint exists, resume from it
if (Test-Path $CheckpointPath) {
    Write-Host "Found existing checkpoint. Resuming..." -ForegroundColor Green
    $ServerArgs += "--resume"
    $ServerArgs += $CheckpointPath
}

$ServerProcess = Start-Process -FilePath $TrainExe -ArgumentList $ServerArgs -NoNewWindow -PassThru

# Give the server a few seconds to load the massive bucket tables into RAM and open the IPC shared memory
Write-Host "Waiting 60 seconds for server to build buckets and open IPC queue..." -ForegroundColor Yellow
Start-Sleep -Seconds 60

# 2b. Start the Workers
Write-Host "Starting $Workers IPC Workers..."
$WorkerProcesses = @()

for ($i = 0; $i -lt $Workers; $i++) {
    $WorkerArgs = @(
        "1000000000",
        "--bucket-config", $ConfigPath,
        "--action-config", $ActionConfigPath,
        "--ipc-worker", $ServerName,
        "--threads", "1",
        "--seed", "$i"
    )
    $P = Start-Process -FilePath $TrainExe -ArgumentList $WorkerArgs -NoNewWindow -PassThru
    $WorkerProcesses += $P
    Start-Sleep -Milliseconds 500
}

# 3. Time tracking loop
$EndTime = (Get-Date).AddHours($Hours)
Write-Host "Training will run until $EndTime" -ForegroundColor Cyan

try {
    while ((Get-Date) -lt $EndTime) {
        if ($ServerProcess.HasExited) {
            Write-Host "IPC Server exited unexpectedly!" -ForegroundColor Red
            break
        }
        
        $AliveWorkers = @($WorkerProcesses | Where-Object { -not $_.HasExited }).Count
        if ($AliveWorkers -lt $Workers) {
            Write-Host "Warning: Only $AliveWorkers / $Workers workers are still running." -ForegroundColor Yellow
        }
        
        Start-Sleep -Seconds 60
    }
}
finally {
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "Time limit reached or interrupted. Shutting down..." -ForegroundColor Cyan
    Write-Host "==================================================" -ForegroundColor Cyan
    
    # Kill the workers immediately
    foreach ($P in $WorkerProcesses) {
        if (-not $P.HasExited) {
            Write-Host "Stopping worker $($P.Id)..."
            Stop-Process -Id $P.Id -Force -ErrorAction SilentlyContinue
        }
    }
    
    # Gently signal the server so it saves its policy and checkpoint
    if (-not $ServerProcess.HasExited) {
        Write-Host "Triggering Server policy save via .stop_ipc_server file..." -ForegroundColor Yellow
        New-Item -ItemType File -Force -Path ".\.stop_ipc_server" | Out-Null
        
        Write-Host "Waiting up to 60 minutes for Server to save enormous tables to disk..."
        $ServerProcess.WaitForExit(3600000)
        
        if (-not $ServerProcess.HasExited) {
            Write-Host "Server took too long to save, forcing kill." -ForegroundColor Red
            Stop-Process -Id $ServerProcess.Id -Force -ErrorAction SilentlyContinue
        } else {
            Write-Host "Server shut down gracefully and saved!" -ForegroundColor Green
        }
    }
    
    Write-Host "Done." -ForegroundColor Cyan
}
