param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot "..\sdkconfig")
)

$resolvedPath = Resolve-Path -LiteralPath $ConfigPath -ErrorAction Stop
$lines = Get-Content -LiteralPath $resolvedPath

$required = @(
    "CONFIG_IDF_TARGET_ESP32S3=y",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y",
    "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y",
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y",
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
    "CONFIG_SPIRAM=y",
    "CONFIG_SPIRAM_MODE_OCT=y",
    "CONFIG_SPIRAM_SPEED_80M=y",
    "CONFIG_SPIRAM_BOOT_INIT=y",
    "CONFIG_SPIRAM_MEMTEST=y",
    "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
    "CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y",
    "CONFIG_ESP32S3_DATA_CACHE_64KB=y",
    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y",
    "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16",
    "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=64",
    "CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=64",
    "CONFIG_ESP_WIFI_TX_BA_WIN=16",
    "CONFIG_ESP_WIFI_RX_BA_WIN=16",
    "CONFIG_ESP_WIFI_EXTRA_IRAM_OPT=y",
    "CONFIG_LWIP_IRAM_OPTIMIZATION=y",
    "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=14400",
    "CONFIG_LWIP_TCP_WND_DEFAULT=14400",
    "CONFIG_LWIP_TCP_RECVMBOX_SIZE=12",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y",
    "CONFIG_CAMERA_CORE1=y",
    "CONFIG_CAMERA_PSRAM_DMA=y"
)

$forbidden = @(
    "CONFIG_ESPTOOLPY_FLASHFREQ_120M=y",
    "CONFIG_SPIRAM_SPEED_120M=y",
    "CONFIG_IDF_EXPERIMENTAL_FEATURES=y"
)

$missing = $required | Where-Object { $_ -notin $lines }
$enabledForbidden = $forbidden | Where-Object { $_ -in $lines }

if ($missing.Count -gt 0) {
    Write-Error ("Missing required performance settings:`n  " + ($missing -join "`n  "))
}

if ($enabledForbidden.Count -gt 0) {
    Write-Error ("Experimental settings must remain disabled:`n  " + ($enabledForbidden -join "`n  "))
}

if ($missing.Count -gt 0 -or $enabledForbidden.Count -gt 0) {
    exit 1
}

Write-Host "ESP32-S3-N16R8 stable performance profile: PASS"
