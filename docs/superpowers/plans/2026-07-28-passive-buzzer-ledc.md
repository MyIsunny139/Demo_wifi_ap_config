# Passive Buzzer LEDC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive the 5V MH-FMD passive buzzer from GPIO14 with a 2 kHz LEDC signal synchronized to the existing WS2812 risk alarm.

**Architecture:** Keep `status_led_task` as the only risk-state owner and add private LEDC initialization/output helpers inside `status_led.c`. Use LEDC timer 1/channel 1 because the camera XCLK already occupies timer 0/channel 0; invert the buzzer output so zero duty leaves the active-low module input high and silent.

**Tech Stack:** ESP-IDF 5.4.2, C, LEDC driver, FreeRTOS, PowerShell source-contract checks.

---

### Task 1: Add a failing buzzer source-contract check

**Files:**
- Create: `tools/check_buzzer_config.ps1`
- Inspect: `components/camera_service/camera_service.c`
- Test: `tools/check_buzzer_config.ps1`

- [ ] **Step 1: Create the static contract check**

Create the following script. It reads `status_led.c`, `camera_service.c`, `status_led/CMakeLists.txt`, and `README.md`, then reports every missing literal contract:

```powershell
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
    @{ Path = 'components/camera_service/camera_service.c'; Text = '.ledc_timer = LEDC_TIMER_0' },
    @{ Path = 'components/camera_service/camera_service.c'; Text = '.ledc_channel = LEDC_CHANNEL_0' },
    @{ Path = 'components/status_led/CMakeLists.txt'; Text = 'driver' },
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
```

- [ ] **Step 2: Run the check and verify RED**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_buzzer_config.ps1
```

Expected: exit code 1 with missing GPIO14/LEDC contract messages because the buzzer implementation does not exist yet.

### Task 2: Implement LEDC buzzer output

**Files:**
- Modify: `components/status_led/status_led.c`
- Modify: `components/status_led/CMakeLists.txt`
- Test: `tools/check_buzzer_config.ps1`

- [ ] **Step 1: Add LEDC constants and initialization**

Include `driver/ledc.h` and add these constants and complete helpers before `apply_output`:

```c
#define BUZZER_GPIO 14
#define BUZZER_FREQUENCY_HZ 2000
#define BUZZER_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define BUZZER_DUTY_ON 512
#define BUZZER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1

static esp_err_t buzzer_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_DUTY_RESOLUTION,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    return ledc_channel_config(&channel_config);
}

static esp_err_t buzzer_set_enabled(bool enabled)
{
    esp_err_t err = ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL,
                                  enabled ? BUZZER_DUTY_ON : 0);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}
```

Return the first ESP-IDF error. With output inversion enabled, duty 0 keeps GPIO14 high and the low-trigger input silent.

- [ ] **Step 2: Add one output helper and integrate every state path**

At the beginning of `apply_output`, add this independent error path before updating the LED:

```c
esp_err_t buzzer_err = buzzer_set_enabled(on && risk != STATUS_LED_RISK_LOW);
if (buzzer_err != ESP_OK) {
    ESP_LOGW(TAG, "Buzzer update failed: %s", esp_err_to_name(buzzer_err));
}
```

Log failures without skipping the WS2812 update. This makes low risk, explicit clear, risk timeout, and normal blink transitions share one sound-control path.

- [ ] **Step 3: Integrate initialization, self-test, and cleanup**

After creating the WS2812 RMT device, initialize the buzzer before creating the queue. On initialization or later allocation/task failures, set duty 0 before releasing LED resources. Change startup self-test to enable the tone for 100 ms, disable it, wait the remaining 200 ms, and then clear the LED. Update the ready log to include `buzzer=GPIO14 2000Hz`.

- [ ] **Step 4: Declare the driver dependency**

Change the component registration to:

```cmake
REQUIRES espressif__led_strip freertos driver
```

- [ ] **Step 5: Run the source-contract check**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_buzzer_config.ps1
```

Expected at this stage: only README contract failures remain.

### Task 3: Document wiring and behavior

**Files:**
- Modify: `README.md`
- Test: `tools/check_buzzer_config.ps1`

- [ ] **Step 1: Update hardware and startup sections**

Add the MH-FMD passive buzzer to the hardware list and describe `VCC -> 5V`, `GND -> GND`, and `IO/IN -> GPIO14`. State that GPIO14 must never receive 5V and that level conversion is required if the module input is internally pulled up to 5V. Update the expected startup log for the combined status output.

- [ ] **Step 2: Replace the LED-only risk table with sound/light behavior**

Document low as LED off and buzzer silent, medium as amber plus 500 ms tone cadence, high as red plus 150 ms tone cadence, and timeout as both outputs off. State that the passive buzzer tone is 2 kHz.

- [ ] **Step 3: Run the contract check and verify GREEN**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_buzzer_config.ps1
```

Expected: `Passive buzzer LEDC contract: PASS`.

### Task 4: Static verification

**Files:**
- Verify: `components/status_led/status_led.c`
- Verify: `components/status_led/CMakeLists.txt`
- Verify: `components/camera_service/camera_service.c`
- Verify: `README.md`
- Verify: `tools/check_buzzer_config.ps1`

- [ ] **Step 1: Check GPIO and LEDC ownership**

Run:

```powershell
rg -n "GPIO14|BUZZER_|LEDC_TIMER_[01]|LEDC_CHANNEL_[01]" components main
```

Expected: camera uses timer/channel 0, buzzer uses GPIO14 and timer/channel 1, and no other component claims GPIO14.

- [ ] **Step 2: Check formatting errors**

Run:

```powershell
git diff --check
```

Expected: no whitespace errors in the files changed for this feature. Existing unrelated worktree changes are not modified.

- [ ] **Step 3: Review the focused diff**

Run:

```powershell
git diff -- components/status_led/status_led.c components/status_led/CMakeLists.txt README.md tools/check_buzzer_config.ps1
```

Expected: only LEDC buzzer integration, sound/light documentation, and the source-contract checker are present. Do not run ESP-IDF compilation, per project instruction.

- [ ] **Step 4: Record hardware acceptance checks**

After flashing on the target board, verify startup tone 100 ms, low-risk silence, medium 500 ms cadence, high 150 ms cadence, and silence after a 5-second command timeout. If the buzzer remains silent, measure GPIO14 for a 2 kHz square wave before changing firmware frequency or wiring.
