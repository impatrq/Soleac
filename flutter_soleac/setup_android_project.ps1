$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectDir

if (-not (Get-Command flutter -ErrorAction SilentlyContinue)) {
    Write-Host "No encuentro flutter en el PATH."
    Write-Host "Abri una terminal nueva o agrega la carpeta flutter\bin al PATH de Windows."
    exit 1
}

$backupDir = Join-Path $projectDir ".setup_backup"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null

$filesToKeep = @(
    "pubspec.yaml",
    "lib\main.dart",
    "lib\soleac_ble_service.dart"
)

foreach ($file in $filesToKeep) {
    $source = Join-Path $projectDir $file
    if (Test-Path $source) {
        $target = Join-Path $backupDir $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -Path $source -Destination $target -Force
    }
}

Write-Host "Generando carpeta Android con flutter create..."
flutter create --platforms=android .

foreach ($file in $filesToKeep) {
    $source = Join-Path $backupDir $file
    if (Test-Path $source) {
        $target = Join-Path $projectDir $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -Path $source -Destination $target -Force
    }
}

$manifestPath = Join-Path $projectDir "android\app\src\main\AndroidManifest.xml"
if (Test-Path $manifestPath) {
    $manifest = Get-Content -Path $manifestPath -Raw
    if ($manifest -notmatch "android.permission.BLUETOOTH_SCAN") {
        $permissions = @"
<uses-feature android:name="android.hardware.bluetooth_le" android:required="true" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION" android:maxSdkVersion="30" />
"@
        $manifest = $manifest -replace "\s*<application", "`r`n$permissions`r`n    <application"
        Set-Content -Path $manifestPath -Value $manifest
    }
}

Write-Host "Instalando dependencias Flutter..."
flutter pub get

Write-Host ""
Write-Host "Listo. Ahora conecta el celular y ejecuta:"
Write-Host "flutter devices"
Write-Host "flutter run"
