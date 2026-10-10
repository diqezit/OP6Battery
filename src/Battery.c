#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>

#include "Op6Battery.h"

// -=-=-=-=-=-=-=-= общие хелперы =-=-=-=-=-=-=-=

static NTSTATUS fill_buf(PVOID Buf, ULONG BufLen, PULONG n,
    const VOID* Src, ULONG Bytes)
{
    if (n) *n = Bytes;
    if (!Buf || BufLen < Bytes)

        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buf, Src, Bytes);
    return STATUS_SUCCESS;
}

#define FILL_OBJ(o) fill_buf(Buf, BufLen, RetLen, &(o), (ULONG)sizeof(o))
#define FILL_STR(s) fill_buf(Buf, BufLen, RetLen, (s), (ULONG)sizeof(s))

// локальная копия снимка под SnapLock
// колбэки читают s а не состояние
// которое в этот момент может менять поток опроса
#define SNAP_READ(s)                    \
    WdfSpinLockAcquire(c->SnapLock);    \
    (s) = c->Snap;                      \
    WdfSpinLockRelease(c->SnapLock)

// запрос с устаревшим тегом batclass не должен доходить до тела колбэка
// (иначе он прочитает уже неактуальный снимок)
// BATTERY_TAG_INVALID отсекаем отдельно: без батареи Tag равен ему же
#define TAG_REQUIRE(ctx, tag_arg)               \
    do {                                        \
        if ((tag_arg) == BATTERY_TAG_INVALID)   \
            return STATUS_NO_SUCH_DEVICE;       \
    } while (0)

// запись под SnapLock: три места делали это руками
static VOID snap_publish(POP6_CTX c, const OP6_SNAP* src)
{
    WdfSpinLockAcquire(c->SnapLock);
    c->Snap = *src;
    WdfSpinLockRelease(c->SnapLock);
}

static VOID snap_set_notify(POP6_CTX c, const BATTERY_NOTIFY* n)
{
    WdfSpinLockAcquire(c->SnapLock);
    c->Notify = *n;
    c->NotifyValid = TRUE;
    WdfSpinLockRelease(c->SnapLock);
}

static VOID snap_clear_notify(POP6_CTX c)
{
    WdfSpinLockAcquire(c->SnapLock);
    c->NotifyValid = FALSE;
    WdfSpinLockRelease(c->SnapLock);
}

// -=-=-=-=-=-=-=-= сведение состояния =-=-=-=-=-=-=-=

VOID BattCommit(POP6_CTX c)
{
    OP6_SNAP* h = &c->Hw;
    static BOOLEAN prevAc = FALSE;

    // дефолты Volt и Temp сюда не пишем пока железо не дало данных
    // читатели видят Valid == FALSE и отдают BATTERY_UNKNOWN_*

    // Если в батарею поступает ток зарядки то внешнее питание есть
    if (h->Curr > AC_CURR_DETECT_MA)
        h->Ac = TRUE;

    /*
        Charging уходит в QStatus как BATTERY_CHARGING
        по нему Windows решает: показывать ли "заряжается" в UI
        применять ли профиль "не экономить", считать ли время до полного

        четыре случая по убыванию очевидности

        1 нет сети или мы погасили заряд по температуре
          ChgOff значит CHG_EN выключен нами и батарея физически
          не получает ток - показать "заряжается" было бы враньем

        2 измеренный разряд (Curr < 0) - НЕ заряжаемся даже при живом кабеле
          главный случай: слабый зарядник (порт ПК) отдает меньше чем
          система потребляет, батарея разряжается при живом VBUS
          если показать "заряжается" - Windows спокоен, профиль "от сети",
          предупреждения выключены и телефон разряжается до отсечки PMIC
          без гибернейта со значком зарядки на экране
          ток это факт - он сильнее флага Ac

        3 измеренный заряд (Curr > 0) - точно заряжаемся, датчик подтверждает

        4 ток не измерен (Curr == CURR_UNMEASURED датчик молчит) - доверяем флагу Ac
          и проценту как раньше, кабель есть и батарея не полная
          запасной путь на шумной шине: read_ibat не прочитал регистр
          но Ac из USB жив
    */
    if (!h->Ac || c->ChgOff) {
        // нет сети или заряд погашен нами по температуре
        h->Charging = FALSE;
    }

    else if (h->Curr < 0) {
        // измеренный разряд - НЕ заряд даже при кабеле
        h->Charging = FALSE;
    }

    else if (h->Curr > 0) {
        // измеренный ток зарядки
        h->Charging = TRUE;
    }

    else {
        // ток не измерен: старая эвристика по проценту
        h->Charging = (BOOLEAN)(h->Soc < SOC_FULL_THRESH);
    }

    /*
        Capacity и Rate считаем здесь один раз
        QStatus и QInfo читают готовые значения из снимка

        Cap = Full * Soc / 100 - абсолютная емкость в мВт-ч для текущего процента
        Windows показывает "осталось X мВт-ч" и строит графики

        Rate = Volt * Curr / 1000 - мощность в мВт (мВ * мА / 1000)
        положительная при заряде отрицательная при разряде

        Curr == CURR_UNMEASURED (не измерен) отдаем BATTERY_UNKNOWN_RATE
        Windows покажет "вычисляем..." вместо мусорного числа
    */
    h->Cap = c->Full * h->Soc / 100;

    if (h->Curr != CURR_UNMEASURED) {
        h->Rate = (LONG)((LONGLONG)h->Volt * h->Curr / 1000);
    }

    else {
        // ток не измерен - мощность неизвестна
        h->Rate = (LONG)BATTERY_UNKNOWN_RATE;
    }

    /*
        BATTERY_CRITICAL это последний шанс Windows сохранить данные
        получив флаг диспетчер питания гибернирует - кэши на диск, чистое засыпание
        без флага телефон умирает жестким сбросом питания
        несохраненные документы теряются, том может повредиться

        критический заряд = три условия одновременно
          1. система работает от батареи
          2. SOC на дне (CRIT_SOC)
          3. напряжение на дне (CRIT_MV)

        условие 1 сложнее чем "кабель выдернут"
        слабый зарядник (порт ПК 500 мА) отдает меньше чем система потребляет
        (500-1200 мА под Windows), батарея разряжается при живом VBUS -
        Ac = TRUE но Curr < 0
        если проверять только Ac эта ситуация невидима
        Windows видит "от сети", ставит профиль "не экономить"
        пороги "от сети" по умолчанию "ничего не делать"
        телефон разряжается до отсечки PMIC без предупреждения
        со значком зарядки на экране
        поэтому "от батареи" = нет кабеля ИЛИ измеренный разряд

        условия 2 и 3 страхуют друг друга
        FG PMI8998 не основной gauge на этом телефоне
        (основной bq27411 по I2C мы его не читаем)
        FG может отдать зависший или заниженный процент -
        верить только проценту значит уснуть на 30% реального заряда
        вольт под нагрузкой проседает на живой батарее
        при 1.5 А читает 3300 на реальных 20-30%
        настоящую пустоту видят оба сразу
        процент на нуле И вольт ниже порога, верим только паре

        три опроса подряд (CRIT_CONFIRM * POLL_MS ~ 6 с)
        одиночный кадр с плохими показаниями это шум
        (рассинхрон FG, пик нагрузки, дребезг датчика)
        три подряд это уже закономерность

        три барьера по убыванию очевидности
        первый - "от сети и не разряжается": критик невозможен, счетчик в 0
        второй - "данных нет или не на дне": рано считать, счетчик в 0
        третий - "признак подтверждается": растим счетчик до CRIT_CONFIRM
        если счетчик на максимуме и признак держится - Crit = TRUE
        пропал (мощный зарядник или FG отдал настоящий процент) -
        первый или второй барьер сбросит Crit = FALSE
    */
    if (h->Ac && h->Curr >= 0) {
        // от сети и батарея не разряжается - критик невозможен
        c->CritCnt = 0;
    }

    else if (!h->Valid || h->Soc > CRIT_SOC || h->Volt > CRIT_MV) {
        // данных нет или показания не на дне - не считаем
        c->CritCnt = 0;
    }

    else if (c->CritCnt < CRIT_CONFIRM) {
        // признак подтверждается - растим счетчик
        c->CritCnt++;

        // переход в критический заряд
        if (c->CritCnt == CRIT_CONFIRM)
            DbgLog("crit: soc=%lu mv=%lu", h->Soc, h->Volt);
    }

    h->Crit = (BOOLEAN)(c->CritCnt >= CRIT_CONFIRM);

    // воткнули или выдернули кабель
    if (h->Ac != prevAc) {
        prevAc = h->Ac;
        DbgLog(h->Ac ? "ac on" : "ac off");
    }

    // QStatus и QInfo работают в потоке IOCTL
    // не должны увидеть наполовину обновленное состояние
    snap_publish(c, h);
}

// -=-=-=-=-=-=-=-= запрос тега =-=-=-=-=-=-=-=

static NTSTATUS QTag(PVOID Ctx, PULONG Tag)
{
    POP6_CTX c = Ctx;

    if (!Tag)
        return STATUS_INVALID_PARAMETER;

    // батарея несъемная
    *Tag = (c->Tag != BATTERY_TAG_INVALID) ? c->Tag : 1;
    return STATUS_SUCCESS;
}

// -=-=-=-=-=-=-=-= запрос информации =-=-=-=-=-=-=-=

static NTSTATUS QInfo(
    PVOID Ctx, ULONG Tag, BATTERY_QUERY_INFORMATION_LEVEL Level,
    LONG AtRate, PVOID Buf, ULONG BufLen, PULONG RetLen)
{
    POP6_CTX c = Ctx;
    OP6_SNAP s;

    if (RetLen)
        *RetLen = 0;

    TAG_REQUIRE(c, Tag);
    SNAP_READ(s);

    switch (Level) {
    case BatteryInformation: {
        // designated init сам зануляет Reserved и CriticalBias
        // technology = 1 значит перезаряжаемая батарея (как в ACPI)
        // chemistry это ровно 4 байта без завершающего NUL а не строка
        ULONG full = c->Full;

        BATTERY_INFORMATION b = {
            .Capabilities = BATTERY_SYSTEM_BATTERY,
            .Technology = 1,
            .Chemistry = { 'L','I','O','N' },
            .DesignedCapacity = c->Design,
            .FullChargedCapacity = full,
            .DefaultAlert1 = full / 20,  // 5%
            .DefaultAlert2 = full / 10,  // 10%
            .CycleCount = c->Cycles,
        };
        return FILL_OBJ(b);
    }

    case BatteryGranularityInformation: {
        BATTERY_REPORTING_SCALE gr = { 1, c->Full };
        return FILL_OBJ(gr);
    }

    case BatteryEstimatedTime: {
        // если запрос пришел с AtRate считаем по нему иначе по снимку
        //
        // rate в мВт (Curr * Volt / 1000) а не в мА
        LONG rate = AtRate;
        if (!rate)
            rate = s.Rate;

        // слабый разряд считаем шумом
        // деление на малый rate дает огромное время и UI покажет мусор
        // BATTERY_UNKNOWN_RATE (ток не измерен) время не дает:
        // как LONG это минимальное число и прошло бы проверку на разряд
        ULONG secs = BATTERY_UNKNOWN_TIME;
        if (s.Valid && !s.Ac && rate != (LONG)BATTERY_UNKNOWN_RATE &&
            rate < -RATE_NOISE_MW)
            secs = (ULONG)((ULONGLONG)s.Cap * 3600 / (ULONGLONG)-(LONGLONG)rate);
        return FILL_OBJ(secs);
    }

    // температура не прочитана: уровень не поддерживаем
    // выдуманную комнатную температуру не отдаем
    case BatteryTemperature: {
        ULONG t = s.Temp;
        if (!t)
            return STATUS_INVALID_DEVICE_REQUEST;
        return FILL_OBJ(t);
    }

    // фиксированные данные батареи BLP657
    case BatteryManufactureName: return FILL_STR(L"OnePlus");
    case BatteryDeviceName:      return FILL_STR(L"BLP657 (OnePlus 6)");
    case BatterySerialNumber:
    case BatteryUniqueID:        return FILL_STR(L"OP6-BLP657-PMI8998");

    // реальная дата изготовления неизвестна: как simbatt для незаданной
    // даты отвечаем STATUS_INVALID_DEVICE_REQUEST а не выдуманной
    case BatteryManufactureDate:
        return STATUS_INVALID_DEVICE_REQUEST;

    default:
        return STATUS_INVALID_PARAMETER;
    }
}

// -=-=-=-=-=-=-=-= запрос состояния =-=-=-=-=-=-=-=

static NTSTATUS QStatus(PVOID Ctx, ULONG Tag, PBATTERY_STATUS St)
{
    POP6_CTX c = Ctx;
    OP6_SNAP s;

    if (!St) return STATUS_INVALID_PARAMETER;

    TAG_REQUIRE(c, Tag);
    SNAP_READ(s);

    // телеметрии нет (SPMI молчит дольше SPMI_FAIL_MAX)
    // батарея несъемная - из системы не исчезает то показания неизвестны
    // обычно не доходим сюда а ета ветка оставлена как защита
    if (!s.Valid) {
        *St = (BATTERY_STATUS){
            .PowerState = 0,
            .Capacity = BATTERY_UNKNOWN_CAPACITY,
            .Voltage = BATTERY_UNKNOWN_VOLTAGE,
            .Rate = BATTERY_UNKNOWN_RATE,
        };
        return STATUS_SUCCESS;
    }

    // по умолчанию считаем что система на батарее
    // пользователь увидит предупреждение раньше
    // чем система неожиданно выключится
    *St = (BATTERY_STATUS){
        .PowerState = BATTERY_DISCHARGING,
        .Capacity = s.Cap,
        .Voltage = s.Volt,
        .Rate = s.Rate,
    };

    /*
        PowerState - главная строка статуса батареи, Windows смотрит на нее
        при каждом QStatus и решает какой профиль питания применять,
        показывать ли предупреждения, считать ли время работы

        возможные состояния
          ON_LINE | CHARGING      от сети батарея заряжается
          ON_LINE                 от сети батарея полная
          DISCHARGING             от батареи (дефолт выше)
          DISCHARGING | CRITICAL  от батареи и почти пустая

        дефолт DISCHARGING уже стоит в struct init выше
        здесь решаем только повышать до ON_LINE или нет
        и добавлять ли CRITICAL

        почему ON_LINE не просто "кабель воткнут" (Ac)
        MSDN: ON_LINE значит "батарея не разряжается" - это обещание системе
        можно не экономить, предупреждения о разряде не нужны
        при живом кабеле батарея может разряжаться: слабый зарядник (порт ПК)
        отдает меньше чем система жрет, VBUS есть но ток в батарею отрицательный
        если поставить ON_LINE при разряде - Windows включает профиль "от сети",
        выключает предупреждения и телефон разряжается до отсечки PMIC
        без гибернейта со значком зарядки на экране
        поэтому ON_LINE требует ОБА условия: кабель есть (Ac) И Curr >= 0

        при Curr == 0 (ток не измерен) доверяем Ac
        датчик молчит но USB-регистры говорят "кабель есть"
        лучше показать "от сети" чем включить экономию на телефоне на заряднике

        CRITICAL добавляется только когда мы НЕ в ON_LINE
        BattCommit считает критик при (!Ac || Curr < 0)
        ON_LINE требует (Ac && Curr >= 0)
        оба флага одновременно невозможны по построению
        else-if гарантирует это даже при баге в BattCommit
    */
    if (s.Ac && s.Curr >= 0) {
        // кабель есть и батарея не разряжается - настоящее питание от сети
        St->PowerState = BATTERY_POWER_ON_LINE;

        if (s.Charging)
            St->PowerState |= BATTERY_CHARGING;
    }
    else if (s.Crit) {
        // нет сети или разряд под кабелем
        // DISCHARGING остается из дефолта, добавляем подтвержденный критик
        St->PowerState |= BATTERY_CRITICAL;
    }

    // система на батарее
    return STATUS_SUCCESS;
}

// -=-=-=-=-=-=-=-= заглушки колбэков =-=-=-=-=-=-=-=

// SetInformation мы не обрабатываем но документация не гарантирует
// что batclass проверяет указатель на NULL
// NULL при ждущем запросе грозит BSOD поэтому нужна заглушка
static NTSTATUS QSetInfo(PVOID Ctx, ULONG Tag,
    BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buf)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buf);
    return STATUS_NOT_SUPPORTED;
}

// EvtTimer будит класс пока заряд за порогом или при смене состояния
// а не на каждом опросе
static NTSTATUS QNotify(PVOID Ctx, ULONG Tag, PBATTERY_NOTIFY N)
{
    POP6_CTX c = Ctx;

    TAG_REQUIRE(c, Tag);
    snap_set_notify(c, N);
    return STATUS_SUCCESS;
}

static NTSTATUS QNotifyOff(PVOID Ctx)
{
    POP6_CTX c = Ctx;

    snap_clear_notify(c);
    return STATUS_SUCCESS;
}

// -=-=-=-=-=-=-=-= привязка к batclass =-=-=-=-=-=-=-=

NTSTATUS BattAttach(POP6_CTX c)
{
    // QueryTag QueryInformation и QueryStatus обязательны
    //
    // SetInformation заглушка SetStatusNotify и DisableStatusNotify
    // хранят пороги
    //
    // MinorVersion 1 это V1_1 только в ней есть поле Fdo
    BATTERY_MINIPORT_INFO_V1_1 m = {
        .MajorVersion = BATTERY_CLASS_MAJOR_VERSION,
        .MinorVersion = 1,
        .Context = c,
        .Pdo = WdfDeviceWdmGetPhysicalDevice(c->Device),
        .QueryTag = QTag,
        .QueryInformation = QInfo,
        .QueryStatus = QStatus,
        .SetInformation = QSetInfo,
        .SetStatusNotify = QNotify,
        .DisableStatusNotify = QNotifyOff,
        .Fdo = WdfDeviceWdmGetDeviceObject(c->Device),
    };
    NTSTATUS st;

    if (!m.Pdo)
        return STATUS_INVALID_DEVICE_STATE;

    // batclass включает интерфейс батареи прямо внутри
    // BatteryClassInitializeDevice и compbatt может прислать
    // IOCTL_BATTERY_QUERY_TAG с другого потока еще до возврата
    // EvtIoctl ждет на ClassLock пока Class не опубликован
    // (так же simbatt держит ClassInitLock)
    WdfWaitLockAcquire(c->ClassLock, NULL);
    st = BatteryClassInitializeDevice((PBATTERY_MINIPORT_INFO)&m, &c->Class);
    if (!NT_SUCCESS(st))
        c->Class = NULL;
    WdfWaitLockRelease(c->ClassLock);

    if (!NT_SUCCESS(st)) {
        // регистрация не прошла
        DbgLog("attach failed st=0x%08X", st);
        return st;
    }

    return st;
}

VOID BattDetach(POP6_CTX c)
{
    PVOID cls;

    /*
        Unload под ClassLock
        EvtIoctl держит тот же лок вокруг BatteryClassIoctl
        иначе выгрузка класса под выполняющимся IOCTL (use-after-free)

        Unload зовет DisableStatusNotify -> SnapLock
        ClassLock изнутри не берется
    */
    WdfWaitLockAcquire(c->ClassLock, NULL);
    cls = InterlockedExchangePointer(&c->Class, NULL);

    if (cls) {
        BatteryClassUnload(cls);
    }

    WdfWaitLockRelease(c->ClassLock);
}
