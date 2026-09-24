param(
    [string]$ProjectRoot = (Join-Path $PSScriptRoot '..')
)

$root = (Resolve-Path -LiteralPath $ProjectRoot -ErrorAction Stop).Path
$contracts = @(
    @{ Path = 'components/status_led/status_led.c'; Text = '#define BUZZER_GPIO 14' },
    @{ Path = 'components/status_led/status_led.c'; Text = '#define BUZZER_FREQUENCY_HZ 2000' },
    @{ Path = 'components/status_led/status_led.c'; Text = '#define BUZZER_LEDC_TIMER LEDC_TIMER_1' },
    @{ Path = 'components/status_led/status_led.c'; Text = '#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1' },
    @{ Path = 'components/status_led/status_led.c'; Text = '.flags.output_invert = true' },
    @{ Path = 'components/status_led/status_led.c'; Text = 'buzzer_set_enabled(on && risk != STATUS_LED_RISK_LOW)' },
    @{ Path = 'components/camera_service/camera_service.c'; Text = '#define CAMERA_SENSOR_NAME "OV2640"' },
    @{ Path = 'components/camera_service/camera_service.c'; Text = '.ledc_timer = LEDC_TIMER_0' },
    @{ Path = 'components/camera_service/camera_service.c'; Text = '.ledc_channel = LEDC_CHANNEL_0' },
    @{ Path = 'components/status_led/CMakeLists.txt'; Text = 'esp_driver_ledc' },
    @{ Path = 'components/status_led/CMakeLists.txt'; Text = 'esp_driver_gpio' },
    @{ Path = 'README.md'; Text = 'GPIO14' },
    @{ Path = 'README.md'; Text = '2 kHz' },
    @{ Path = 'README.md'; Text = '5V' }
)

$missing = foreach ($contract in $contracts) {
    $path = Join-Path $root $contract.Path
    $content = Get-Content -Raw -LiteralPath $path -ErrorAction Stop
    if (-not $content.Contains($contract.Text)) {
        "$($contract.Path): $($contract.Text)"
    }
}

if ($missing.Count -gt 0) {
    Write-Error ("Missing passive buzzer contracts:`n  " + ($missing -join "`n  "))
    exit 1
}

Write-Host 'Passive buzzer LEDC contract: PASS'
