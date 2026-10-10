# OP6Battery

KMDF driver for battery, charging, RGB notification indicator and flashlight
for OnePlus 6 (SDM845 / PMI8998) on Windows 11 ARM64.

Hardware ID: `ACPI\OP6B0001`

---

## Overview

The driver polls PMI8998 over SPMI (`qcspmi8480.sys`, `QCOM_SPMI` on `\Device\Spmi`) and publishes battery state through `batclass.sys`. If the bus is not open or silent, data comes from a bootloader mailbox at physical `0x9FFE1000`, signature `BATT`.

Poll interval: 2 s (`POLL_MS`). `HwInit` polls once immediately from mailbox. First timer tick after 4 s (`FIRST_POLL_MS`). SPMI is first touched after `SPMI_INIT_DELAY` because the bus is shared with USB Wi-Fi and BT.

All light control is in `Flashlight.c`: flashlight (FLASH, slave 2) and RGB indicator (LPG + Tri-LED, slave 3). `PMI8998.c` calls `LedUpdateBattery()` in the poll loop. Register blocks do not overlap, so flashlight and indicator are independent of charger and fuel gauge.

---

## Features

### Battery

- SOC, voltage, current, temperature, discharge rate in Windows UI
- Charge/network status; critical: SOC <= 1% or voltage <= 3400 mV for 3 polls (~6 s) -> `BATTERY_CRITICAL`
- `BATTERY_NOTIFY` thresholds update UI immediately
- Capacity in mWh: design 12705, real 11088 (BLP657), 240 cycles default
- Validation: voltage 2500-4500 mV, temperature 250-350 K; bad samples dropped

### Charging

- Software charge enable: `CHG_CFG2` sets `CHG_EN` to software control
- FCC 2000 mA, precharge 1500 mA, float 4.35 V, input limit 2000 mA
- Thermal window 0-45 C with 3 C hysteresis
- Charger watchdog petted every poll while charging
- Charge disabled by thermal stop is restored on unload (`HwRestoreCharge`)

### RGB indicator

- Auto color by battery
- PWM 64 Hz (32 kHz clock, 9-bit)
- PMIC write only on state change
- Turned off on driver unload
- Manual user-mode color control not implemented (`LedManualOverride` reserved)

### Flashlight

- IOCTL `IOCTL_OP6_TORCH_SET` (0xA10), brightness 0-100
- Up to 200 mA per rear diode, IRES step 12.5 mA
- Torch mode, no safety timer
- IOCTL works immediately after driver load
- Turned off on driver unload

### Diagnostics

- Log `C:\Users\Public\op6_debug.log`: lifecycle, LED/flashlight events, charger dump each poll

---

## Project layout

```
OP6Battery/
  src/
    Driver.c
    Battery.c
    PMI8998.c
    Flashlight.c
    Debug.c
    Op6Battery.h
    OP6Battery.inf
    Install.bat
    ACPI/
      op6_battery.asl
      SSDT.aml
  OP6Battery.vcxproj
  OP6Battery.sln
```

Build output:

```
Bin/ARM64/Release/
  OP6Battery.sys
  OP6Battery.inf
  op6battery.cat
  OP6Battery.cer
  OP6Battery.pdb
  SSDT.aml
  Install.bat
```

---

## Registers and masks

Register addresses, masks and values are in `Op6Battery.h` enums. Code uses names only (`FLASH_STROBE_CTRL`, `LPG_VAL_EN_ON`, `TRI_LED_EN_RED`, ...). Values match mainline Qualcomm drivers and live device logs.

SPMI slaves:

| Slave | Blocks | Writer |
|---|---|---|
| 2 (`SLAVE`) | CHGR 0x1000, DCDC 0x1100, USBIN 0x1300, MISC 0x1600, FG 0x4000, FLASH 0xD300 | `PMI8998.c`, `Flashlight.c` |
| 3 (`LED_SLAVE`) | LPG 0xB300-0xB500, Tri-LED 0xD045-0xD046 | `Flashlight.c` |

Main groups:

```
CHGR   CHG_STS 1006, CHG_EN 1042, CHG_FAST 1043, CHG_CFG2 1051,
       CHG_PRE 1060, CHG_FCC 1061, CHG_FLOAT 1070
USBIN  USB_IN_STATUS 1306, APSD_STS 1307, APSD_RES 1308, USB_STS 1310,
       USB_SUSP 1340, USB_APSD 1341, USB_FLOAT 1363, USB_LOAD 1365,
       USB_OPT 1366, USB_ICL 1370
MISC   CHG_WD_PET 1643 (write 0x01)
FG     FG_SOC 4009, FG_TEMP 4150, FG_VBAT 41A0, FG_IBAT 41A2
FLASH  SAFETY_TIMER D340, MAX_CURRENT D341, LED1_CUR D342, LED2_CUR D343,
       CLAMP_CUR D345, IRES D346, STROBE_CFG D347, STROBE_CTRL D348
LPG    red B500 (ch 5), green B400 (ch 4), blue B300 (ch 3); offsets 41-47
TriLED SRC D045 (VPH_PWR), EN D046 (R 0x80, G 0x40, B 0x20)
```

---

## RGB indicator

LPG hardware PWM. Channels: red 5, green 4, blue 3 (slave 3). Duty 0-511 split across two registers. Tri-LED (`0xD045`/`0xD046`) connects channels to VPH_PWR.

32 kHz clock and 9-bit resolution -> 64 Hz PWM. Workaround for PMI8998 silicon bug: rewrite duty after channel enable.

Auto color (`LedUpdateBattery` from `HwPoll`):

| Condition | Color |
|---|---|
| cable, SOC < 20% | red (255, 0, 0) |
| cable, 20% <= SOC < 90% | yellow (255, 140, 0) |
| cable, SOC >= 90% | green (0, 255, 0) |
| no cable | off |

PMIC write only on state change (`LastLedState`). Indicator lights ~6-10 s after driver start. Turned off on unload (`HwExit` -> `LedSetRgb(c, 0, 0, 0)`).

---

## Flashlight

Torch mode: 200 mA per diode, no short safety timer. Brightness 1-100 -> raw current in 12.5 mA IRES steps (max 16). Enable order: currents/limits, IRES, software strobe. `STROBE_CTRL` last; failure turns strobe off.

| Item | Value |
|---|---|
| IOCTL | `IOCTL_OP6_TORCH_SET` = `CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA10, METHOD_BUFFERED, FILE_ANY_ACCESS)` |
| Input | `OP6_TORCH_REQ { UCHAR Brightness; }` (0 = off, 1-100 = brightness) |
| Output | none |
| Name | `\\.\OP6Torch` |
| Interface | `GUID_DEVINTERFACE_OP6_TORCH` `{7C3E9A12-4B6F-4E81-9D20-A1B2C3D4E5F6}` |

Example:

```c
#define IOCTL_OP6_TORCH_SET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA10, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct { BYTE Brightness; } OP6_TORCH_REQ;   /* 0 = off, 1..100 */

HANDLE h = CreateFileW(L"\\\\.\\OP6Torch", GENERIC_READ | GENERIC_WRITE,
                       0, NULL, OPEN_EXISTING, 0, NULL);
if (h != INVALID_HANDLE_VALUE) {
    OP6_TORCH_REQ req = { 60 };
    DWORD done = 0;
    DeviceIoControl(h, IOCTL_OP6_TORCH_SET, &req, sizeof(req),
                    NULL, 0, &done, NULL);
    CloseHandle(h);
}
```

---

## Charging unlock sequence

`HwPoll` runs this once when external power appears (`chg_seq`, order as in XBL):

| # | Register | Write | Meaning |
|---|---|---|---|
| 1 | `CHG_CFG2` (1051) | mask 0xC0 <- 0 | `CHG_EN` source = register |
| 2 | `CHG_FLOAT` (1070) | 0x73 | 4.35 V |
| 3 | `CHG_PRE` (1060) | 0x3C | precharge 1500 mA |
| 4 | `CHG_FCC` (1061) | 0x50 | charge 2000 mA |
| 5 | `USB_FLOAT` (1363) | bit 0 | FLOAT detect -> SDP |
| 6 | `USB_LOAD` (1365) | bit 4 | ICL override after APSD |
| 7 | `USB_OPT` (1366) | bit 0 | charge from input |
| 8 | `USB_ICL` (1370) | 0x50 | input limit 2000 mA |
| 9 | `USB_SUSP` (1340) | bit 0 <- 0 | remove suspend |
| 10 | `CHG_FAST` (1043) | bit 0 <- 1 | allow fast charge |

Then wait for APSD done and enable `CHG_EN`. If charger is DISABLE (watchdog reset), make 0 -> 1 edge first.

Start conditions: valid telemetry, open bus, temperature in thermal window. Temperature from FG PMI8998 (0x4150, 0.1 K) and optionally bq27411 over I2C if SSDT provides SPB `_CRS`. Base ASL has no resource, so only PMI8998 is used.

---

## Debug log

`DbgLog` -> `C:\Users\Public\op6_debug.log` (append, PASSIVE_LEVEL only).

| Line | Meaning |
|---|---|
| `torch: on raw=... pct=...` / `torch: off` | flashlight |
| `hw_identify: ...` | PMI8998 identification |
| `EvtSmInit` / `EvtSmSuspend` / `EvtSmRestart` / `EvtSmCleanup` | lifecycle |
| `mailbox: ...` | bootloader shared memory |
| `chg restore failed` | charge restore failed on unload |
| `wrb failed reg=...` / `torch_wr failed reg=...` | PMIC write failure |

Log is not rotated. Delete before long debug sessions.

---

## Requirements

- Visual Studio 2022
- Desktop development with C++
- MSVC v143 ARM64/ARM64EC build tools
- Windows Driver Kit for VS 2022
  https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk

Toolset: `WindowsKernelModeDriver10.0`. Windows SDK alone is not enough; install WDK separately.

Phone must have SPMI provider (`qcspmi8480.sys`, BOOT_START; `qcspmi850.sys` also tested).

---

## Build

1. Open `OP6Battery.sln`.
2. Select `Release | ARM64`.
3. Build > Build Solution.

PostBuild copies `src\OP6Battery.inf`, `src\ACPI\SSDT.aml`, `src\Install.bat`, and `op6battery.cat` to `Bin\ARM64\Release\`.

If VS reports `WindowsKernelModeDriver10.0 not found`, install WDK and reopen the solution.

---

## Test signing

Build signs `OP6Battery.sys` and `op6battery.cat` automatically (SHA256, project test cert).

Manual:

```
signtool sign /v /fd SHA256 /n "OP6BatteryTestCert" Bin\ARM64\Release\OP6Battery.sys
signtool sign /v /fd SHA256 /n "OP6BatteryTestCert" Bin\ARM64\Release\op6battery.cat
```

Do not commit `.pfx` or `.cer`.

---

## Install preparation

1. Secure Boot off.
2. Administrator rights.
3. SSDT loaded via UEFI Shell; it creates `ACPI\OP6B0001`. Without it driver installs but does not bind (code 31).

Important: INF and SSDT HWID must match. In this tree `OP6Battery.inf` binds `ACPI\BQ027411`, while ASL/SSDT declare `OP6B0001` (and `Install.bat` checks `OP6B0001`). Fix INF or ASL before install and rebuild.

---

## Load SSDT (once)

SSDT must be loaded by UEFI before Windows starts.

1. Copy `SSDT.aml` to FAT32 USB drive.
2. Reboot, open UEFI Shell.
3. Run:

```
map
fs0:
ls
load SSDT.aml
exit
```

4. Windows starts with `ACPI\OP6B0001`.

Check:

```
pnputil /enum-devices | findstr /C:"OP6B0001"
```

Empty output = SSDT not loaded. SSDT is not persistent; reload after each reboot unless firmware supports permanent injection.

---

## Install

From `Bin\ARM64\Release\` as administrator:

```
Install.bat
```

Script:

1. `bcdedit /set testsigning on`
2. Installs `OP6Battery.cer` to Root and TrustedPublisher
3. Checks `ACPI\OP6B0001`
4. `pnputil /add-driver OP6Battery.inf /install`
5. `pnputil /scan-devices`
6. Checks binding

If SSDT is missing, script warns and prints UEFI Shell instructions. Driver stays in DriverStore and binds after SSDT load + rerun.

---

## Check

1. Device Manager > Batteries > OnePlus 6 Battery, or:

```
Get-PnpDevice -InstanceId "ACPI\OP6B0001"
```

Status should be `OK`.

2. Indicator: after ~10 s RGB shows battery color. Plug/unplug cable changes color on next poll.
3. Flashlight: use example above.
4. Log: `C:\Users\Public\op6_debug.log` (`torch:` on toggle, `Led:` on color change).

---

## Remove

```
pnputil /delete-driver OP6Battery.inf /uninstall
```

Then remove device from Device Manager, or:

```
pnputil /remove-device "ACPI\OP6B0001"
```

Reinstall: run `Install.bat` again. On unload driver turns off flashlight/indicator and restores charge if it disabled it.

---

## Notes

- Kernel driver; bugs can bugcheck. Test signing only for development.
- Capacity reported in mWh, not percent.
- UEFI mailbox: `0x9FFE1000`, signature `BATT` (`0x54544142`).
- SPMI interface GUID: `{72c96e71-eb2d-48e3-99ac-9894a59f9c4c}` (`qcspmi8480.sys`).
- KMDF 1.33 (matches INF `KmdfLibraryVersion` and vcxproj `KMDF_VERSION_MINOR`).
- Flashlight (FLASH 0xD3xx) and indicator (LPG 0xB3xx, Tri-LED 0xD045-0xD046) do not overlap.
- Manual indicator color from user mode not implemented; only auto color by battery.
