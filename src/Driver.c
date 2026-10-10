#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <spb.h>
#include <initguid.h>

#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>

#include "Op6Battery.h"

// -=-=-=-=-=-=-=-= контекст и инициализация =-=-=-=-=-=-=-=

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OP6_CTX, GetCtx)

DRIVER_INITIALIZE DriverEntry;

static NTSTATUS IrpComplete(PIRP Irp, NTSTATUS st, ULONG_PTR info)
{
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

// -=-=-=-=-=-=-=-= интерфейс SPMI шины =-=-=-=-=-=-=-=

/*
    интерфейс Qualcomm SPMI Bus
    публикует qcspmi8480.sys (в BSP для WoA)
    через QueryForInterface на \Device\Spmi получаем QCOM_SPMI
    с колбэками ReadLong/WriteLong - через них идет вся работа с PMI8998

    не менять: значение фиксировано в драйвере шины

    идентификатор интерфейса
    отдает таблицу функций QCOM_SPMI с чтением и записью регистров
*/
static const GUID GUID_SPMI =
    { 0x72c96e71, 0xeb2d, 0x48e3, { 0x99, 0xac, 0x98, 0x94, 0xa5, 0x9f, 0x9c, 0x4c } };

// -=-=-=-=-=-=-=-= диспетчер IOCTL =-=-=-=-=-=-=-=

/*
    препроцесс-колбэк IRP_MJ_DEVICE_CONTROL
    KMDF отдает его раньше своих очередей поэтому сериализацию делаем сами
    иначе два IOCTL полезут в batclass одновременно

    BatteryClassIoctl синхронно дергает QTag QInfo QStatus и сам завершает IRP
    c->Class это хэндл batclass

    BattDetach при выгрузке под ClassLock обнуляет его и зовет BatteryClassUnload
    без лока тут окно use-after-free

    лок тот же что в BattAttach и BattDetach дедлока нет все Q-колбэки внутри
    берут только SnapLock чужой IRP уходит вниз к PDO вне лока
*/
static NTSTATUS IoctlTorch(POP6_CTX c, PIRP Irp, ULONG inLen)
{
    POP6_TORCH_REQ req = (POP6_TORCH_REQ)Irp->AssociatedIrp.SystemBuffer;

    if (inLen < sizeof(*req))
        return IrpComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);

    if (!req || !c)
        return IrpComplete(Irp, STATUS_INVALID_PARAMETER, 0);

    return IrpComplete(Irp, TorchSet(c, req->Brightness), 0);
}

static NTSTATUS EvtIoctl(WDFDEVICE Dev, PIRP Irp)
{
    POP6_CTX c = GetCtx(Dev);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS st = STATUS_NOT_SUPPORTED;

    PAGED_CODE();

    if (code == IOCTL_OP6_TORCH_SET)
        return IoctlTorch(c, Irp, sp->Parameters.DeviceIoControl.InputBufferLength);

    if (c != NULL) {
        WdfWaitLockAcquire(c->ClassLock, NULL);
        if (c->Class != NULL)
            st = BatteryClassIoctl(c->Class, Irp);
        WdfWaitLockRelease(c->ClassLock);
    }

    // не наш IOCTL - отдаем вниз по стеку
    if (st == STATUS_NOT_SUPPORTED) {
        IoSkipCurrentIrpStackLocation(Irp);
        st = WdfDeviceWdmDispatchPreprocessedIrp(Dev, Irp);
    }

    return st;
}

// -=-=-=-=-=-=-=-= периодический опрос =-=-=-=-=-=-=-=

// таймер запускают и останавливают 4 колбэка
//
// Timer может быть NULL если WdfTimerCreate провалилась в EvtAdd
//
// таймер разовый - KMDF не разрешает periodic вместе с Passive
// поэтому EvtTimer сам заводит следующий тик
// Run = TRUE пока EvtTimer должен перезапускать себя
static VOID TimerRun(POP6_CTX c, ULONG ms)
{
    if (c->Timer) {
        c->Run = TRUE;
        WdfTimerStart(c->Timer, WDF_REL_TIMEOUT_IN_MS(ms));
    }
}

static VOID TimerHalt(POP6_CTX c)
{
    // Run сбрасываем до Stop иначе колбэк успеет перезапустить таймер
    c->Run = FALSE;

    // до освобождения класса и железа ждем
    // тут только TRUE
    // FALSE НЕ применять!!
    // иначе SPMI и mailbox - опрос продолжит работу с мертвыми ресурсами
    if (c->Timer)
        WdfTimerStop(c->Timer, TRUE);
}

static VOID EvtTimer(WDFTIMER Timer)
{
    POP6_CTX c = GetCtx(WdfTimerGetParentObject(Timer));

    if (!c->Run)
        return;

    // SPMI проверяет read_hw
    HwPoll(c);

    WdfSpinLockAcquire(c->SnapLock);
    BATTERY_NOTIFY n  = c->Notify;
    BOOLEAN        nv = c->NotifyValid;
    WdfSpinLockRelease(c->SnapLock);

    // power manager будим пока заряд за порогом BATTERY_NOTIFY
    // и при смене состояния
    BOOLEAN low  = nv && (n.LowCapacity  != BATTERY_UNKNOWN_CAPACITY) && (c->Hw.Cap <= n.LowCapacity);
    BOOLEAN high = nv && (n.HighCapacity != BATTERY_UNKNOWN_CAPACITY) && (c->Hw.Cap >= n.HighCapacity);

    // подтвержденный критический заряд (BattCommit CRIT_CONFIRM опросов)
    // класс должен узнать сразу независимо от выставленных им порогов
    // (контракт BatteryClassStatusNotify)
    BOOLEAN crit = c->Hw.Crit;

    BOOLEAN capChanged = (low != c->LowFlag) || (high != c->HighFlag);
    BOOLEAN hwChanged  = (c->Hw.Ac != c->PrevAc) ||
                         (c->Hw.Charging != c->PrevChg) ||
                         (c->Hw.Valid != c->PrevValid) ||
                         (crit != c->PrevCrit);

    // пока порогов нет будим на каждом тике
    // пороги по уровню а не по фронту (BatteryMiniSetStatusNotify:
    // "любой раз когда батарея вне диапазона") выход из-за порога
    // тоже будит чтобы класс увидел возврат в диапазон
    BOOLEAN wake = !nv || low || high || capChanged || hwChanged;

    // ClassLock здесь не берем: BatteryClassStatusNotify синхронно зовет
    // QStatus и завершает ждущие IRP а лок не рекурсивный
    // Class не выгрузится под нами: EvtSmCleanup останавливает таймер
    // (WdfTimerStop с Wait = TRUE) до BattDetach а BattAttach
    // публикует Class до первого TimerRun
    if (wake) {
        PVOID cls = InterlockedCompareExchangePointer(&c->Class, NULL, NULL);
        if (cls)
            BatteryClassStatusNotify(cls);
    }

    c->LowFlag      = low;
    c->HighFlag     = high;
    c->PrevAc       = c->Hw.Ac;
    c->PrevChg      = c->Hw.Charging;
    c->PrevValid    = c->Hw.Valid;
    c->PrevCrit     = crit;

    // Run мог сброситься за время опроса
    if (c->Run)
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
}

// -=-=-=-=-=-=-=-= I2C-ресурс =-=-=-=-=-=-=-=

/*
    ищем в ресурсах устройства I2C-соединение (I2cSerialBus из ACPI)
    по нему получаем путь к resource hub и открываем таргет к bq27411
    таргет дочерний к устройству (ParentObject = Dev)

    без этого таргета read_temp молчит: FG_TEMP у PMI8998 на этом
    телефоне мертв, температура только с bq27411
    TempFresh остается FALSE, unlock не включает заряд,
    а при уже идущем заряде HwPoll гасит CHG_EN (летим вслепую)

    I2C опционален для PnP: нет ресурса / Create / Open не удался -
    устройство все равно стартует, зарядник остается как оставил загрузчик

    ReleaseHardware зовется до EvtSmCleanup (HwExit)
    канал закрываем здесь по контракту KMDF
    read_temp в HwExit после этого не пройдет - CHG_EN останется погашен
    это безопаснее чем включать заряд вслепую
*/
static NTSTATUS EvtPrepareHardware(
    WDFDEVICE Dev,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated)
{
    POP6_CTX c = GetCtx(Dev);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_IO_TARGET_OPEN_PARAMS op;
    WDF_OBJECT_ATTRIBUTES at;
    ULONG i, count;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    if (ResourcesTranslated == NULL)
        return STATUS_SUCCESS;

    // rebalance: фреймворк зовет PrepareHardware снова
    // снимаем старый target перед Create - иначе потеряем хэндл
    WDF_IOTARGET_DELETE_SAFE(c->I2cTarget);

    WDF_OBJECT_ATTRIBUTES_INIT(&at);
    at.ParentObject = Dev;

    count = WdfCmResourceListGetCount(ResourcesTranslated);

    for (i = 0; i < count; i++) {
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc->Type != CmResourceTypeConnection ||
            desc->u.Connection.Class != CM_RESOURCE_CONNECTION_CLASS_SERIAL ||
            desc->u.Connection.Type  != CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
            continue;

        RESOURCE_HUB_CREATE_PATH_FROM_ID(
            &path, desc->u.Connection.IdLowPart, desc->u.Connection.IdHighPart);

        st = WdfIoTargetCreate(Dev, &at, &c->I2cTarget);
        if (!NT_SUCCESS(st)) {
            c->I2cTarget = NULL;
            DbgLog("i2c: create 0x%08X", (ULONG)st);
            break;
        }

        I2C_TARGET_OPEN_PARAMS_INIT(op, path);

        st = WdfIoTargetOpen(c->I2cTarget, &op);
        if (!NT_SUCCESS(st)) {
            WDF_IOTARGET_DELETE_SAFE(c->I2cTarget);
            DbgLog("i2c: open 0x%08X", (ULONG)st);
            break;
        }

        DbgLog("i2c: opened");
        break;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Dev, WDFCMRESLIST ResourcesTranslated)
{
    POP6_CTX c = GetCtx(Dev);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    WDF_IOTARGET_DELETE_SAFE(c->I2cTarget);
    return STATUS_SUCCESS;
}

/*
    прочитать n байт регистра reg с bq27411
    write-then-read одной SPB-последовательностью
    TRUE только если драйвер шины вернул не меньше n байт

    пара к spmi_rd: без I2cTarget сразу FALSE
    буфер вызывающего зануляем чтобы частичный мусор не ушел вверх
*/
BOOLEAN i2c_rd(POP6_CTX c, UCHAR reg, PUCHAR buf, ULONG n)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) xfer;
    WDF_MEMORY_DESCRIPTOR mem;
    ULONG_PTR bytes = 0;
    NTSTATUS st;

    if (!c->I2cTarget) return FALSE;

    RtlZeroMemory(buf, n);

    SPB_TRANSFER_LIST_INIT(&xfer.List, 2);
    xfer.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, &reg, sizeof(reg));
#pragma warning(suppress: 6201)
    xfer.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, buf, n);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, &xfer, sizeof(xfer));

    st = WdfIoTargetSendIoctlSynchronously(
        c->I2cTarget, NULL, IOCTL_SPB_EXECUTE_SEQUENCE,
        &mem, NULL, NULL, &bytes);

    return (BOOLEAN)(NT_SUCCESS(st) && bytes >= n);
}

// -=-=-=-=-=-=-=-= очистка SPMI =-=-=-=-=-=-=-=

/*
    закрыть шину и забыть про нее
    отдаем интерфейс закрываем канал и обнуляем все что с ним связано
    чтобы никто не вызвал мертвый указатель

    порядок важен
      1) Deref говорим шинному драйверу что интерфейс нам больше не нужен
         ссылку мы брали в ensure_spmi значит ее надо вернуть
      2) закрываем канал (target) если он был открыт
      3) зануляем указатели на функции spmi_rd и wrb увидят NULL
         и честно ответят "шины нет" вместо падения
      4) сбрасываем флаги которые зависят от шины

    is_open = TRUE если WdfIoTargetOpen прошел иначе target не открыт
*/
VOID SpmiTeardown(POP6_CTX c, BOOLEAN is_open)
{

    // SpmiIface смотрит внутрь c->Spmi (живой контекст устройства)
    // Deref зануляет хэндл сессии в старших 32 битах
    if (c->SpmiDeref) c->SpmiDeref(c->SpmiIface);

    /*
        target не удаляем а только закрываем
        новый объект каждый опрос дергает PnP

        target это объект-"труба" до шинного драйвера
        создать его один раз и потом только открывать-закрывать дешевле
        чем каждый раз создавать новый
        окончательно он удаляется только в HwExit
    */
    if (c->Target) {
        if (is_open && WdfIoTargetGetState(c->Target) != WdfIoTargetClosed)
            WdfIoTargetClose(c->Target);
    }

    c->SpmiRead  = NULL;
    c->SpmiWrite = NULL;
    c->SpmiBus   = NULL;
    c->SpmiDeref = NULL;
    c->SpmiIface = NULL;

    /*
        интерфейса нет значит зарядник после его возврата настроим заново
        и железо опознаем заново
        ChgOff не трогаем CHG_EN все еще погашен
    */
    c->Fast   = FALSE;
    c->HwOk   = FALSE;
    c->Rev1Wa = FALSE;
}

// -=-=-=-=-=-=-=-= открытие SPMI =-=-=-=-=-=-=-=

/*
    открыть SPMI по одному из двух имен устройства

    у шинного драйвера есть "файл-устройство" и мы открываем его как обычный файл
    имен два пробуем по порядку и берем первое которое открылось
      \Device\Spmi\SUPERUSER и \Device\Spmi

    доступ GENERIC_READ | GENERIC_WRITE потому что нам нужно и читать и писать регистры
    ShareAccess разрешает другим клиентам шины открыть то же устройство одновременно с нами

    возвращает TRUE если какое то имя открылось
*/
static BOOLEAN try_open_spmi(POP6_CTX c)
{
    static const PCWSTR name[] = {
        L"\\Device\\Spmi\\SUPERUSER",
        L"\\Device\\Spmi"
    };

    UNICODE_STRING u;
    WDF_IO_TARGET_OPEN_PARAMS op;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(name); i++) {

        // параметры собираем заново на каждое имя
        // внутри op остается указатель на u конкретного имени поэтому init в цикле
        RtlInitUnicodeString(&u, name[i]);

        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&op, &u, GENERIC_READ | GENERIC_WRITE);

        op.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

        st = WdfIoTargetOpen(c->Target, &op);

        if (NT_SUCCESS(st)) {
            DbgLog("spmi: opened name[%lu]", i);
            return TRUE;
        }
        DbgLog("spmi: open failed name[%lu] st=0x%08X", i, (ULONG)st);
    }

    return FALSE;
}

/*
    открывает SPMI если еще не открыт иначе ничего не делает
    при неудаче отступает экспоненциально чтобы не штурмовать PnP каждый опрос

    это дверь в шину ее зовет каждый опрос из read_hw
    шаги
      1) уже открыта выходим
      2) идет пауза после неудачи отнимаем 1 и выходим
      3) создаем target если его еще нет
      4) открываем канал по имени try_open_spmi
      5) просим у шины интерфейс QCOM_SPMI QueryForInterface
      6) проверяем что интерфейс отдали целиком и с нужными функциями
      7) запоминаем функции чтения/записи и опознаем железо hw_identify
    если любой шаг сломался прыгаем в cleanup закрываем все и взводим паузу

    про паузы backoff
    шина может быть еще не готова она грузится параллельно с нами
    долбиться в нее каждые 2 секунды бессмысленно
    поэтому после каждой неудачи пауза удваивается
    пропускаем 1 опрос потом 2 потом 4 8 16 и наконец 32
    32 опроса это SPMI_RETRY_MAX (около 64 секунд) больше пауза не растет
    после успешного открытия счетчик паузы обнуляется
*/
VOID ensure_spmi(POP6_CTX c)
{
    /*
        буфер интерфейса обязан жить в OP6_CTX а не на стеке
        провайдер qcspmi хранит адрес нашего буфера до самого Deref

        Context указывает на саму структуру (самоссылка в декомпиляте)
        а провайдерский список в контексте файла держит этот же адрес
        локальная переменная = разыменование мёртвого стека
        в SpmiTeardown и в очистке провайдера при закрытии target (CWE-825)
    */
    QCOM_SPMI* iface = &c->Spmi;
    BOOLEAN opened = FALSE;
    NTSTATUS st = STATUS_UNSUCCESSFUL;

    /*
        при удалении шины (query-remove или удаление устройства)
        фреймворк сам закрывает target
        после этого звать указатели интерфейса нельзя

        target еще Started значит шина жива и делать нечего
        target не Started значит шина ушла

        снимаем наше состояние через SpmiTeardown с is_open = FALSE
        (target уже закрыт фреймворком повторно закрывать не надо)
        и идем обычным путем повторного открытия
    */
    if (c->SpmiRead) {
        if (WdfIoTargetGetState(c->Target) == WdfIoTargetStarted)
            return;

        /*
            шину сняли а target закрыл фреймворк
            ссылку взятую QueryForInterface обязан вернуть импортер:
            пока Deref не вызван провайдер не может выгрузиться
        */
        DbgLog("spmi: closed state=%d", WdfIoTargetGetState(c->Target));

        SpmiTeardown(c, FALSE);
    }

    // пауза после прошлой неудачи еще не вышла
    if (c->SpmiRetry) {
        c->SpmiRetry--;
        return;
    }

    // target создаем один раз за жизнь драйвера
    if (!c->Target && !NT_SUCCESS(WdfIoTargetCreate(
            c->Device, WDF_NO_OBJECT_ATTRIBUTES, &c->Target)))
        goto cleanup;

    opened = try_open_spmi(c);
    if (!opened) goto cleanup;

    /*
        визитку собираем только здесь перед запросом
        ранние выходы выше (живой интерфейс + backoff-пауза) не должны затирать c->Spmi
        пока интерфейс активен провайдер хранит его адрес
        к этому моменту старый уже снят (Deref в Teardown)
    */
    RtlZeroMemory(iface, sizeof(*iface));
    iface->Size    = sizeof(*iface);
    iface->Version = SPMI_IFACE_VERSION;

    /*
      Запрос интерфейса QCOM_SPMI

      InterfaceSpecificData: аргумент по договоренности
      фреймворк не придает ему смысла а просто передает в IRP_MN_QUERY_INTERFACE

      NULL не безопасный fallback
      qcspmi850 ждет Target
      Передаем Target и только Target
    */
    st = WdfIoTargetQueryForInterface(
            c->Target, &GUID_SPMI,
            (PINTERFACE)iface, sizeof(*iface), SPMI_IFACE_VERSION, c->Target);

    if (!NT_SUCCESS(st)) {
        DbgLog("spmi: query iface failed: 0x%08X", (ULONG)st);
        goto cleanup;
    }

    DbgLog("spmi: ctx_self=%u", (ULONG)(iface->Context == (PVOID)iface));

    /*
        ссылка на интерфейс взята с этого момента ее снимает SpmiTeardown
        Deref и контекст запоминаем сразу до проверок
        если проверка не пройдет cleanup вызовет SpmiTeardown и тот вернет ссылку
    */
    c->SpmiDeref = iface->Deref;
    c->SpmiIface = iface->Context;

    /*
        Size проверяем по последнему нужному полю а не на равенство
        шина может вернуть свой размер структуры
        нам нужны поля до WriteLong включительно
    */
    if (iface->Size < RTL_SIZEOF_THROUGH_FIELD(QCOM_SPMI, WriteLong) ||
        !iface->ReadLong || !iface->WriteLong) {
        DbgLog("spmi: iface fields invalid Size=%u", iface->Size);
        goto cleanup;
    }

    c->SpmiBus     = iface->SpmiContext;
    c->SpmiRead    = iface->ReadLong;
    c->SpmiWrite   = iface->WriteLong;

    DbgLog("spmi: connected");

    /*
        HwOk только для wrb
        колин драйвер батарею видит без PERPH_SUBTYPE
        результат не критичен если опознать не вышло просто не пишем в зарядник
        а читать батарею продолжаем как обычно
    */
    (VOID)hw_identify(c);

    c->SpmiBackoff = 0;
    return;

cleanup:
    SpmiTeardown(c, opened);

    /*
        пауза 1 2 4 ... SPMI_RETRY_MAX пропущенных опросов до новой попытки
        из 0 получается 1 из 1 получается 2 и так далее
    */
    c->SpmiBackoff = OP6_CLAMP(c->SpmiBackoff * 2, 1, SPMI_RETRY_MAX);
    c->SpmiRetry   = c->SpmiBackoff;

    // попытка открыть шину не удалась как отказ шины
    DbgLog("spmi: open failed backoff=%lu", c->SpmiRetry);
}

// -=-=-=-=-=-=-=-= mailbox =-=-=-=-=-=-=-=

/*
    снимает маппинг shared-memory загрузчика (mailbox) если он есть
    чтобы освободить ресурсы ядра
    безопасно при c->Mailbox == NULL

    маппинг это окно через которое ядро видит кусок физической памяти
    закончили пользоваться окно надо закрыть (MmUnmapIoSpace)
    иначе утечка

    после вызова Mailbox становится NULL и read_mb больше ничего не вернет
    вызывается из трех мест read_hw (когда шина заработала или умерла) и HwExit
*/
VOID mailbox_unmap(POP6_CTX c)
{
    if (c->Mailbox) {

        DbgLog("mailbox_unmap");

        MmUnmapIoSpace(c->Mailbox, sizeof(MB));
        c->Mailbox = NULL;
    }
}

// -=-=-=-=-=-=-=-= жизненный цикл железа =-=-=-=-=-=-=-=

/*
    вызывается один раз при старте устройства из EvtSmInit

    открывает окно в память mailbox
    сразу делает первый опрос не дожидаясь таймера

    зачем первый опрос прямо здесь
    после HwInit драйвер регистрируется в batclass и Windows может спросить тег батареи почти сразу
    тег появляется только когда есть валидные данные (см комментарий в HwPoll)
    поэтому они должны быть готовы до регистрации
*/
VOID HwInit(POP6_CTX c)
{
    PHYSICAL_ADDRESS pa;
    volatile MB* m;

    /*
    маппинг shared-memory (mailbox) оставленного UEFI/ABL для передачи состояния PMIC/Fuel Gauge
    MmNonCached отключает кэширование чтобы CPU читал актуальные данные из RAM а не из L1/L2 кэша
     */
    pa.QuadPart = MB_PA;
    m = MmMapIoSpace(pa, sizeof(MB), MmNonCached);

    if (!m) {
        // маппинг не удался это не авария
        // read_mb умеет работать с NULL и просто скажет mailbox нет
        DbgLog("mailbox: map failed");
    } else if (m->Sig != MB_SIG || !m->Present) {
        /*
            mailbox не наш или батареи там нет
            значит регион загрузчика нам не нужен
            снимаем маппинг сразу а не таскаем чужую физическую память каждый опрос
        */
        DbgLog("mailbox: bad sig=0x%08X present=%u", m->Sig, m->Present);

        MmUnmapIoSpace((PVOID)m, sizeof(MB));
        m = NULL;
    }

    c->Mailbox = (PVOID)m;

    /*
        ждем SPMI_INIT_DELAY опросов прежде чем тронуть шину

        SPMI не наша шина она общая на весь телефон
        через нее же в первые минуты после загрузки поднимают себя USB Wi-Fi и BT
        сунуться туда раньше положенного сбивает их настройку USB с Wi-Fi отваливаются до перезагрузки

        за это время ожидания батарея живет из mailbox
        дольше SPMI_FAIL_MAX промахов он не переживет (см ветку B)
        HwPoll ниже отработает сразу но шину не тронет
        только спишет счетчик SpmiRetry а данные возьмет из mailbox
    */
    c->LastLedState = 0xFF;
    c->LedManualOverride = FALSE;
    c->SpmiRetry = SPMI_INIT_DELAY;
    HwPoll(c);
}

/*
    вызывается один раз при остановке устройства из EvtSmCleanup
    порядок уборки такой сначала то что требует открытой шины
      1) вернуть зарядку если мы ее выключили иначе она так и останется выключенной после выгрузки драйвера
      2) погасить фонарик и светодиод пока шина еще открыта
      3) закрыть шину
      4) удалить target окончательно
      5) закрыть окно в mailbox
    к этому моменту таймер уже остановлен так что с опросом мы не пересечемся
*/
VOID HwExit(POP6_CTX c)
{
    HwRestoreCharge(c);
    TorchOff(c);
    LedSetRgb(c, 0, 0, 0);

    SpmiTeardown(c, c->SpmiRead != NULL);

    // target удаляем один раз здесь (канал уже закрыл Teardown)
    WDF_DELETE_OBJECT_SAFE(c->Target);

    mailbox_unmap(c);
}

// -=-=-=-=-=-=-=-= self-managed I/O =-=-=-=-=-=-=-=

static NTSTATUS EvtSmInit(WDFDEVICE Dev)
{
    POP6_CTX c = GetCtx(Dev);
    NTSTATUS st;

    HwInit(c);

    // интерфейс батареи регистрирует batclass по PDO
    st = BattAttach(c);
    if (!NT_SUCCESS(st)) {
        DbgLog("EvtSmInit: BattAttach 0x%08X", (ULONG)st);
        HwExit(c);
        return st;
    }

    // первый опрос делаем быстрее чтобы UI не мигал пустой батареей
    TimerRun(c, FIRST_POLL_MS);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtSmSuspend(WDFDEVICE Dev)
{
    DbgLog("EvtSmSuspend: begin");
    TimerHalt(GetCtx(Dev));
    DbgLog("EvtSmSuspend: end");
    return STATUS_SUCCESS;
}

static NTSTATUS EvtSmRestart(WDFDEVICE Dev)
{
    TimerRun(GetCtx(Dev), POLL_MS);
    return STATUS_SUCCESS;
}

static VOID EvtSmCleanup(WDFDEVICE Dev)
{
    POP6_CTX c = GetCtx(Dev);

    // порядок обратный EvtSmInit
    // таймер использует класс и железо поэтому останавливаем его первым
    DbgLog("EvtSmCleanup: begin");

    TimerHalt(c);
    BattDetach(c);
    HwExit(c);

    DbgLog("EvtSmCleanup: end");
}

// -=-=-=-=-=-=-=-= создание устройства =-=-=-=-=-=-=-=

static NTSTATUS EvtAdd(WDFDRIVER Drv, PWDFDEVICE_INIT Di)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES at;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    POP6_CTX c;
    UNICODE_STRING torchSym;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Drv);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware      = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware      = EvtReleaseHardware;
    pnp.EvtDeviceSelfManagedIoInit    = EvtSmInit;
    pnp.EvtDeviceSelfManagedIoSuspend = EvtSmSuspend;
    pnp.EvtDeviceSelfManagedIoRestart = EvtSmRestart;
    pnp.EvtDeviceSelfManagedIoCleanup = EvtSmCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(Di, &pnp);

    st = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        Di, EvtIoctl, IRP_MJ_DEVICE_CONTROL, NULL, 0);
    RETURN_IF_ERROR(st);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&at, OP6_CTX);
    st = WdfDeviceCreate(&Di, &at, &dev);
    RETURN_IF_ERROR(st);

    // ошибка из EvtAdd после WdfDeviceCreate: framework сам удаляет
    // устройство вместе с дочерними объектами и EvtSmInit не вызывает
    // WdfObjectDelete(dev) звать нельзя - для WDFDEVICE это запрещено
    c = GetCtx(dev);
    c->Device    = dev;
    c->I2cTarget = NULL;
    c->Tag       = 1;                     // телефон всегда имеет батарею

    // пока не отдает эти значения знач берем константы
    c->Full   = FULL_MWH;
    c->Design = DESIGN_MWH;
    c->Cycles = DEFAULT_CYCLES;

    (VOID)WdfDeviceCreateDeviceInterface(dev, &GUID_DEVINTERFACE_OP6_TORCH, NULL);

    RtlInitUnicodeString(&torchSym, OP6_TORCH_DOS_DEVICE_NAME);
    (VOID)WdfDeviceCreateSymbolicLink(dev, &torchSym);

    // локи и таймер создаются с общими атрибутами
    WDF_OBJECT_ATTRIBUTES_INIT(&at);
    at.ParentObject = dev;

    // ClassLock защищает Class от гонки EvtIoctl и EvtSysCtl с BattAttach/BattDetach
    st = WdfWaitLockCreate(&at, &c->ClassLock);
    RETURN_IF_ERROR(st);

    st = WdfSpinLockCreate(&at, &c->SnapLock);
    RETURN_IF_ERROR(st);

    // Passive ставим после создания локов чтобы они не получили его из общего at
    WDF_TIMER_CONFIG_INIT(&tc, EvtTimer);
    tc.AutomaticSerialization = FALSE;
    at.ExecutionLevel = WdfExecutionLevelPassive;

    // Timer зануляем явно: TimerRun и TimerHalt проверяют его на NULL
    c->Timer = NULL;
    st = WdfTimerCreate(&tc, &at, &c->Timer);
    if (!NT_SUCCESS(st)) {
        c->Timer = NULL;       // не оставляем мусор в хэндле
        DbgLog("EvtAdd: timer 0x%08X", (ULONG)st);
        return st;
    }

    return STATUS_SUCCESS;
}

// -=-=-=-=-=-=-=-= точка входа =-=-=-=-=-=-=-=

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG cfg;
    NTSTATUS st;

    WDF_DRIVER_CONFIG_INIT(&cfg, EvtAdd);
    st = WdfDriverCreate(Drv, Path, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
    DbgLog("DriverEntry: 0x%08X", (ULONG)st);
    return st;
}
