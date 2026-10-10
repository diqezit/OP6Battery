/*
 * OnePlus 6 Custom Battery ACPI Secondary System Description Table (SSDT)
 * Hardware ID: ACPI\OP6B0001
 * Target: Windows 11 ARM64 (OP6Battery.sys)
 */

DefinitionBlock ("SSDT.aml", "SSDT", 2, "OP6   ", "OP6BATT ", 1)
{
    External (\_SB, DeviceObj)

    Scope (\_SB)
    {
        Device (OP6B)
        {
            Name (_HID, "OP6B0001")                  // Matches OP6Battery.inf Hardware ID
            Name (_UID, 0x01)                        // Unique ID 1
            Name (_STR, Unicode ("OnePlus 6 Battery Controller (PMI8998)"))

            // Device Status:
            // 0x0F = Present (bit 0) | Enabled (bit 1) | ShowInUI (bit 2) | Functional (bit 3)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
