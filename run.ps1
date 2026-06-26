# ==============================================================================
#  WynlandOS - QEMU Launch Script (Windows PowerShell)
#
#  Usage:
#    .\run.ps1                    # Normal run
#    .\run.ps1 -Debug             # Debug mode (GDB on port 1234)
#    .\run.ps1 -QemuPath "C:\..." # Custom QEMU path
# ==============================================================================

param(
    [switch]$Debug,
    [string]$QemuPath = "",
    [switch]$NoNet
)

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir   = Join-Path $ScriptDir "build"
$OvmfFw     = Join-Path $ScriptDir "tools\ovmf\OVMF.fd"
$DiskImage  = Join-Path $BuildDir  "wynland.img"

# -- Find QEMU -----------------------------------------------------------------

if ($QemuPath -eq "") {
    # Common QEMU install locations on Windows
    $QemuPaths = @(
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
        "$env:LOCALAPPDATA\Programs\qemu\qemu-system-x86_64.exe"
    )
    
    foreach ($path in $QemuPaths) {
        if (Test-Path $path) {
            $QemuPath = $path
            break
        }
    }
    
    # Try PATH
    if ($QemuPath -eq "") {
        $qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
        if ($qemu) {
            $QemuPath = $qemu.Source
        }
    }
    
    if ($QemuPath -eq "") {
        Write-Host "ERROR: qemu-system-x86_64 not found!" -ForegroundColor Red
        Write-Host "Install QEMU: https://www.qemu.org/download/#windows"
        exit 1
    }
}

# -- Check files ---------------------------------------------------------------

if (-not (Test-Path $OvmfFw)) {
    Write-Host "ERROR: OVMF firmware not found at $OvmfFw" -ForegroundColor Red
    Write-Host "Download OVMF.fd and place it in tools\ovmf\"
    exit 1
}

if (-not (Test-Path $DiskImage)) {
    Write-Host "ERROR: Disk image not found. Build first with WSL: make" -ForegroundColor Red
    exit 1
}

# -- Build QEMU arguments ------------------------------------------------------

$QemuArgs = @(
    "-machine", "q35,kernel-irqchip=off",
    "-cpu", "qemu64",
    "-accel", "whpx",
    "-accel", "tcg",
    "-m", "256M",
    "-bios", $OvmfFw,
    "-drive", "file=$DiskImage,format=raw",
    "-vga", "none",
    "-device", "virtio-gpu-pci,disable-legacy=off,disable-modern=on",
    "-serial", "stdio",
    "-no-reboot",
    "-no-shutdown"
)

if (-not $NoNet) {
    $QemuArgs += @(
        "-device", "virtio-net-pci,netdev=net0",
        "-netdev", "user,id=net0"
    )
}

if ($Debug) {
    $QemuArgs += @("-S", "-s", "-d", "int,cpu_reset")
    Write-Host "Debug mode: connect GDB to localhost:1234" -ForegroundColor Yellow
}

# -- Launch --------------------------------------------------------------------

Write-Host ""
Write-Host "  ======================================" -ForegroundColor Cyan
Write-Host "  |      WynlandOS - Starting QEMU       |" -ForegroundColor Cyan
Write-Host "  ======================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  QEMU: $QemuPath" -ForegroundColor DarkGray
Write-Host ""

& $QemuPath $QemuArgs
