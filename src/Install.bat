@echo off
setlocal

title OP6Battery Installer

net session 1>nul 2>nul
if errorlevel 1 (
    echo [ERROR] Run as Administrator.
    echo.
    echo Press any key to close...
    pause 1>nul
    exit /b 1
)

set "HERE=%~dp0"
set "STAGE=%ProgramFiles%\OP6Battery"

if /I "%~1"=="/uninstall" goto :uninstall

set "EXP_SHA="
if /I "%~1"=="/sha256" set "EXP_SHA=%~2"

if not exist "%HERE%OP6Battery.sys" (
    echo [ERROR] OP6Battery.sys not found in "%HERE%"
    echo.
    pause 1>nul
    exit /b 1
)
if not exist "%HERE%OP6Battery.inf" (
    echo [ERROR] OP6Battery.inf not found in "%HERE%"
    echo.
    pause 1>nul
    exit /b 1
)

echo [0/5] Staging files to "%STAGE%"...
if /I "%HERE%"=="%STAGE%\" goto :staged
if not exist "%STAGE%" mkdir "%STAGE%" 1>nul 2>nul
set "STAGE_OK=1"
for %%F in (OP6Battery.sys OP6Battery.inf op6battery.cat OP6Battery.cer) do (
    if exist "%HERE%%%F" (
        copy /Y "%HERE%%%F" "%STAGE%\" 1>nul 2>nul
        if errorlevel 1 set "STAGE_OK=0"
    )
)
if "%STAGE_OK%"=="0" (
    echo [ERROR] Could not copy files to "%STAGE%".
    echo.
    pause 1>nul
    exit /b 1
)
:staged
echo [OK] Files staged. Installing from the protected folder only.

set "SYS=%STAGE%\OP6Battery.sys"
set "INF=%STAGE%\OP6Battery.inf"
set "CER=%STAGE%\OP6Battery.cer"

if not defined EXP_SHA goto :nohash
set "ACT_SHA="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%SYS%" SHA256') do if not defined ACT_SHA set "ACT_SHA=%%H"
if not defined ACT_SHA set "ACT_SHA=none"
set "ACT_SHA=%ACT_SHA: =%"
if /I not "%ACT_SHA%"=="%EXP_SHA%" (
    echo [ERROR] SHA256 mismatch: OP6Battery.sys was modified or corrupted.
    echo Expected: %EXP_SHA%
    echo Actual:   %ACT_SHA%
    echo.
    pause 1>nul
    exit /b 1
)
echo [OK] OP6Battery.sys SHA256 matches.
goto :hashdone
:nohash
echo [WARN] No /sha256 given, file integrity not verified.
echo Usage: Install.bat /sha256 ^<hash from the release page^>
:hashdone

echo.
echo [1/5] Enabling Test Signing Mode...
echo [INFO] If BitLocker is on, keep the recovery key at hand:
echo        changing boot settings may trigger BitLocker recovery.
bcdedit /set testsigning on 1>nul 2>nul
if errorlevel 1 (
    echo [WARN] Could not enable testsigning.
    echo Secure Boot may be ON. Disable it in BIOS.
) else (
    echo [OK] Test signing enabled.
)

echo.
echo [2/5] Installing test certificate...
if exist "%CER%" (
    certutil -addstore Root "%CER%" 1>nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Failed to add certificate to Root store.
        echo.
        pause 1>nul
        exit /b 1
    )
    certutil -addstore TrustedPublisher "%CER%" 1>nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Failed to add to TrustedPublisher store.
        echo.
        pause 1>nul
        exit /b 1
    )
    echo [OK] Certificate installed.
) else (
    echo [WARN] OP6Battery.cer not found, skipping.
)

echo.
echo [3/5] Checking for ACPI\OP6B0001 device...
set "SSDT_MISSING=1"
pnputil /enum-devices 2>nul | findstr /C:"OP6B0001" 1>nul 2>nul
if not errorlevel 1 (
    set "SSDT_MISSING=0"
    echo [OK] Device present.
) else (
    echo [WARN] ACPI\OP6B0001 not found.
)

echo.
echo [4/5] Installing driver via pnputil...
pnputil /add-driver "%INF%" /install
set "PNP_RC=%errorlevel%"
if not "%PNP_RC%"=="0" (
    echo [WARN] pnputil /add-driver returned %PNP_RC%.
)

set "DRV_OK=0"
pnputil /enum-drivers 2>nul | findstr /I "op6battery" >nul 2>&1
if not errorlevel 1 set "DRV_OK=1"

if "%DRV_OK%"=="0" (
    echo [ERROR] Driver was not added to DriverStore.
    echo Common cause: .cat does not match .inf hashes.
    echo Rebuild the driver and try again.
    echo.
    pause 1>nul
    exit /b 1
)
echo [OK] Driver added to DriverStore.
if "%PNP_RC%"=="3010" echo [INFO] Reboot required to finish installation.

pnputil /scan-devices 1>nul 2>nul
echo [OK] PnP rescan triggered.

echo.
echo [5/5] Verifying driver binding...
set "BIND_FAILED=0"
pnputil /enum-devices /problem 2>nul | findstr /C:"OP6B0001" 1>nul 2>nul
if not errorlevel 1 set "BIND_FAILED=1"

if "%SSDT_MISSING%"=="1" (
    echo [FAIL] Device ACPI\OP6B0001 not present. SSDT not loaded.
) else if "%BIND_FAILED%"=="1" (
    echo [FAIL] Device present but driver failed to bind.
    echo Run: pnputil /enum-devices /problem /deviceid "ACPI\OP6B0001"
) else (
    echo [SUCCESS] Driver installed and bound.
    echo Open Device Manager - Batteries - OnePlus 6 Battery.
)

if "%SSDT_MISSING%"=="1" (
    echo.
    echo ==============================================================
    echo SSDT LOADING INSTRUCTIONS ^(do this ONCE^)
    echo ==============================================================
    echo.
    echo "load" in UEFI Shell cannot install an ACPI table ^(UEFI drivers only^).
    echo Use ONE of these:
    echo  a^) add SSDT.aml to the ACPI tables of your UEFI build and reflash;
    echo  b^) use an EFI app/driver that calls EFI_ACPI_TABLE_PROTOCOL.InstallAcpiTable
    echo     before Windows Boot Manager starts.
    echo Then boot Windows and run Install.bat again.
    echo.
)

echo.
echo To roll back: Install.bat /uninstall
echo.
echo Done.
echo.
echo Press any key to close...
pause 1>nul
exit /b 0

:uninstall
echo [1/4] Removing OP6Battery driver packages...
set "DEL_ANY=0"
set "LAST="
for /f "tokens=1,* delims=:" %%A in ('pnputil /enum-drivers 2^>nul') do (
    for /f "tokens=*" %%V in ("%%B") do (
        echo %%V| findstr /R /I /X "oem[0-9]*\.inf" 1>nul 2>nul && set "LAST=%%V"
        if /I "%%V"=="op6battery.inf" call :deldrv
    )
)
if "%DEL_ANY%"=="0" echo [INFO] No OP6Battery driver package found.

echo.
echo [2/4] Removing test certificate...
set "UCER=%STAGE%\OP6Battery.cer"
if not exist "%UCER%" set "UCER=%HERE%OP6Battery.cer"
if not exist "%UCER%" goto :nocer
set "THUMB="
for /f "delims=" %%T in ('powershell -NoProfile -Command "(New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 '%UCER%').Thumbprint" 2^>nul') do set "THUMB=%%T"
if not defined THUMB goto :nocer
certutil -delstore Root %THUMB% 1>nul 2>nul
certutil -delstore TrustedPublisher %THUMB% 1>nul 2>nul
echo [OK] Certificate %THUMB% removed from Root and TrustedPublisher.
goto :cerdone
:nocer
echo [WARN] OP6Battery.cer not found, remove the certificate manually ^(certlm.msc^).
:cerdone

echo.
echo [3/4] Disabling Test Signing Mode...
echo [INFO] Skip this if other test-signed drivers still need it.
bcdedit /set testsigning off 1>nul 2>nul
if errorlevel 1 (
    echo [WARN] Could not disable testsigning.
) else (
    echo [OK] Test signing disabled.
)

echo.
echo [4/4] Removing staged files...
if /I "%HERE%"=="%STAGE%\" (
    echo [WARN] Running from "%STAGE%", delete this folder manually.
) else (
    if exist "%STAGE%" rd /s /q "%STAGE%" 1>nul 2>nul
    echo [OK] Staged files removed.
)

echo.
echo Done. Reboot to finish.
echo.
echo Press any key to close...
pause 1>nul
exit /b 0

:deldrv
if not defined LAST exit /b 0
pnputil /delete-driver %LAST% /uninstall /force 1>nul 2>nul
if errorlevel 1 (
    echo [WARN] Could not remove %LAST%.
) else (
    echo [OK] Removed %LAST%.
)
set "DEL_ANY=1"
set "LAST="
exit /b 0
