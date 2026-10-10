#include "Op6Battery.h"
#include <ntstrsafe.h>

VOID DbgLog(PCSTR format, ...)
{
    CHAR buffer[512];
    size_t length = 0;
    va_list args;
    NTSTATUS status;
    HANDLE fileHandle;
    IO_STATUS_BLOCK ioStatus;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING path = RTL_CONSTANT_STRING(
        L"\\DosDevices\\C:\\Users\\Public\\op6_debug.log");

    // passive_level специальные kernel APC должны быть включены
    if (format == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || KeAreAllApcsDisabled())
        return;

    // оставляем два байта для CRLF
    va_start(args, format);
    status = RtlStringCbVPrintfA(
        buffer, sizeof(buffer) - 2, format, args);
    va_end(args);

    if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
        return;

    if (!NT_SUCCESS(RtlStringCbCatA(buffer, sizeof(buffer), "\r\n")) ||
        !NT_SUCCESS(RtlStringCbLengthA(buffer, sizeof(buffer), &length)))
        return;

    InitializeObjectAttributes(
        &attributes, &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateFile(
        &fileHandle, FILE_APPEND_DATA | SYNCHRONIZE,
        &attributes, &ioStatus, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (!NT_SUCCESS(status))
        return;

    // FILE_APPEND_DATA уже обеспечивает запись в конец
    (VOID)ZwWriteFile(
        fileHandle, NULL, NULL, NULL, &ioStatus,
        buffer, (ULONG)length, NULL, NULL);

    (VOID)ZwClose(fileHandle);
}
