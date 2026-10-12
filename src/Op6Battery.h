#ifndef OP6BATTERY_H
#define OP6BATTERY_H

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>   // BATTERY_TAG_INVALID, BATTERY_NOTIFY, BATTERY_UNKNOWN_*

// -=-=-=-=-=-=-=-= хелперы =-=-=-=-=-=-=-=

// удаление WDF-объекта с обнулением
#define WDF_DELETE_OBJECT_SAFE(p) \
    do { \
        if (p) { \
            WdfObjectDelete((WDFOBJECT)(p)); \
            (p) = NULL; \
        } \
    } while (0)

// то же + Close, если I/O target еще открыт
#define WDF_IOTARGET_DELETE_SAFE(p) \
    do { \
        if (p) { \
            if (WdfIoTargetGetState((WDFIOTARGET)(p)) != WdfIoTargetClosed) \
                WdfIoTargetClose((WDFIOTARGET)(p)); \
            WdfObjectDelete((WDFOBJECT)(p)); \
            (p) = NULL; \
        } \
    } while (0)

#define RETURN_IF_ERROR(st) \
    do { if (!NT_SUCCESS(st)) return (st); } while (0)

// инициализация параметров открытия I2C target с фиксированными флагами
#define I2C_TARGET_OPEN_PARAMS_INIT(params, path) \
    do { \
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&(params), &(path), GENERIC_READ | GENERIC_WRITE); \
        (params).ShareAccess       = FILE_SHARE_READ | FILE_SHARE_WRITE; \
        (params).CreateDisposition = FILE_OPEN; \
        (params).FileInformation   = FILE_NON_DIRECTORY_FILE; \
    } while (0)

// сборка USHORT из двух байтов в little-endian порядке
#define MAKE_USHORT_LE(lo, hi) ((USHORT)((USHORT)(lo) | ((USHORT)(hi) << 8)))

// регистр температуры BQ27411 (команда 0x02, формат 0.1 K)
#define BQ27411_REG_TEMP 0x02

// регистры емкости bq27411 в мАч LSB-first как температура
// адреса и формат из mainline bq27xxx_battery.c (мапа bq27411 = bq27421)
#define BQ27411_REG_RC   0x0C     // RemainingCapacity остаток
#define BQ27411_REG_FCC  0x0E     // FullChargeCapacity выученная
#define BQ27411_REG_DCAP 0x3C     // DesignCapacity паспортная

// зажать значение в диапазон [lo, hi]
// используется при вычислениях SOC ICL и подобных
#define OP6_CLAMP(v, lo, hi) min(max((v), (lo)), (hi))

// проверка что значение в диапазон [lo, hi] включительно
// используется при валидации показаний железа
#define IN_RANGE(v, lo, hi) ((v) >= (lo) && (v) <= (hi))

// -=-=-=-=-=-=-=-= преобразование единиц FG =-=-=-=-=-=-=-=

#define FG_VBAT_LSB_NV  122070ULL   // цена одного шага VBAT 122.07 мкВ (нановольты)

#define FG_IBAT_LSB_NA  488281LL    // цена одного шага IBAT 488.281 мкА (наноамперы)

#define UNIT_NANO_DIV   1000000ULL  // делитель нано -> милли единиц

#define SOC_RAW_FULL    0xFF        // сырое значение полной батареи

#define SOC_PCT_FULL    100         // процент полной батареи

#define SOC_RAW_MIN     1           // нижняя граница сырого диапазона SOC

#define SOC_SPREAD      98          // растяжка 1..254 в 1..99 и округление к ближайшему
#define SOC_ROUND       0x7E
#define SOC_DIV         0xFD

#define CHG_WD_PET_VAL  0x01        // значение записи в watchdog зарядника (BARK_BITE_WDOG_PET)

// -=-=-=-=-=-=-=-= таймер =-=-=-=-=-=-=-=

// период опроса железа в миллисекундах
// раз в столько батарея обновляется в UI
#define POLL_MS 2000

/*
    задержка до первого опроса после старта
    4 секунды дают модулю Wi-Fi (WCN3990) время завершить
    инициализацию на шине SPMI без коллизий
*/
#define FIRST_POLL_MS 4000

/*
    сколько раз пробуем перечитать SOC
    пока два байта регистра не совпадут между собой
    не совпали за все попытки - значение не принимается
*/
#define SOC_TRIES 3

// -=-=-=-=-=-=-=-= пороги BattCommit =-=-=-=-=-=-=-=

/*
    ток выше этого без флага Ac считаем признаком зарядки
    50 мА с запасом перекрывает шум датчика
    используется только когда USB_STS в этом опросе не прочитан
*/
#define AC_CURR_DETECT_MA 50

// SOC выше этого - батарея считается полной
// 99 а не 100 потому что литий никогда не достигает ровно 100
#define SOC_FULL_THRESH 99

// -=-=-=-=-=-=-=-= пороги QStatus =-=-=-=-=-=-=-=

/*
    SOC не выше этого - выставляем BATTERY_CRITICAL
    система начнет аварийное завершение
    достижимо только при Valid == TRUE и только после CRIT_CONFIRM опросов
*/
#define CRIT_SOC 1

/*
    напряжение не выше этого на батарее - тоже критический заряд
    страховка SOC: на OnePlus 6 основной gauge внешний (bq27411/bq27541
    по I2C) FG PMI8998 как источник SOC не подтвержден

    2850 мВ это DEFAULT_EMPTY_VOLT_MV у FG gen3 - точка "батарея пуста"
    на ней предупреждать уже поздно нужен запас до снятия питания
    3400 мВ - порог загрузчика для старта HLOS (BootToHLOSThresholdInMv)
    ниже вендор не доверяет ОС работать вообще
*/
#define CRIT_MV 3400

/*
    сколько опросов подряд критический признак должен держаться
    прежде чем уйдет BATTERY_CRITICAL (3 * POLL_MS ~ 6 с)
    одиночный отсчет (рассинхрон FG просадка под нагрузкой) не счет
*/
#define CRIT_CONFIRM 3

// ток разряда в мА ниже этого - шум датчика
// оценку времени работы по нему не считаем
#define RATE_NOISE_MA 50

// номинал банки BLP657 по спецификации 3.85 В
// множитель мА·ч bq27411 -> мВт·ч Windows
// живой вольтаж нельзя: Full кэшируется на теге раз и навсегда
// а Cap отдается каждый опрос - рассинхрон даст Cap > Full и % за 100
#define BATT_NOMINAL_MV 3850

// -=-=-=-=-=-=-=-= эвристики состояния =-=-=-=-=-=-=-=

// сырой ток IBAT ниже этого - батарея заряжается
// используется только когда регистр USB_STS не отвечает
#define IBAT_RAW_CHG_THRESH -10

/*
    маска битов 0..5 регистра USB_IN_STATUS (0x1306)
    каждый бит = наличие входного напряжения на рельсе
    любое ненулевое значение = VBUS присутствует
    как в smblib_get_prop_usb_present
*/
#define USB_IN_VOLT_MASK 0x3F

/*
    ноль тока означает "датчик не ответил"
    настоящий ровно нулевой ток тоже попадает сюда
    на расчеты не влияет: Rate в этом случае BATTERY_UNKNOWN_RATE
*/
#define CURR_UNMEASURED 0

// бит USB_STS (USBIN INT_RT_STS) означающий что кабель воткнут
// USBIN_PLUGIN_RT_STS_BIT = BIT(4) как в smblib_get_prop_usb_present
#define USB_STS_PRESENT_MASK 0x10

// -=-=-=-=-=-=-=-= SPMI =-=-=-=-=-=-=-=

// SPMI slave id PMI8998
// в старшем байте адреса регистра при обращении по шине
#define SLAVE 2

// слейв в старшем байте адреса регистр в младшем
#define SPMI_ADDR(reg) (((ULONG)(SLAVE) << 16) | (reg))

/*
    единственный код успеха у колбэков qcspmi
    ошибки положительные (5 FAILURE 6 DENIED 7 DROPPED 12 контекст)
    все проходят NT_SUCCESS поэтому проверка st == SPMI_SUCCESS
    а не NT_SUCCESS(st) который пропустил бы их как успех
*/
#define SPMI_SUCCESS 0

/*
    версия интерфейса QCOM_SPMI которую ожидает провайдер
    qcspmi8480 принял 1 (подтверждено логом "connected")
*/
#define SPMI_IFACE_VERSION 1

/*
    сколько опросов пропустить перед первой попыткой тронуть шину
    через нее поднимают себя USB Wi-Fi и BT - чтоб не перебить периферию
*/
#define SPMI_INIT_DELAY 2

// сколько опросов подряд SPMI может молчать прежде чем данные
// считаются протухшими а интерфейс переоткрывается
// молчанием считается и опрос где VBAT прочитан а SOC нет
#define SPMI_FAIL_MAX 5

// максимальная пауза между попытками открыть SPMI в опросах
// каждая неудача удваивает паузу до этого предела
#define SPMI_RETRY_MAX 32 // 32 * POLL_MS ~ 64 с

// -=-=-=-=-=-=-=-= зарядник =-=-=-=-=-=-=-=

/*
    ток быстрой зарядки (CHG_FCC) в мА
    2000 мА (2.0 А при 5 В = 10 Вт стандартный режим зарядки OnePlus 6)
*/
#define FCC_MA 2000

// нижний предел тока при клампе (FCC_REG)
// ниже 500 мА заряд становится нестабильным
#define ICL_MIN_MA 500

// верхний предел тока при клампе (FCC_REG)
// выше 2500 мА PMI8998 не тянет без перегрева
#define ICL_MAX_MA 2500

// шаг регистров тока в мА
// значение делится на этот шаг перед записью
#define ICL_STEP_MA 25

/*
    значение регистра CHG_FCC: FCC_MA в пределах
    [ICL_MIN_MA, ICL_MAX_MA] в шагах ICL_STEP_MA
    константа времени компиляции поэтому идет прямо в таблицу unlock
*/
#define FCC_REG ((UCHAR)(OP6_CLAMP(FCC_MA, ICL_MIN_MA, ICL_MAX_MA) / ICL_STEP_MA))

// лимит входного тока с зарядника (ICL) в 0x1370
// 2000 мА / 25 мА = 80 = 0x50
#define ICL_REG_VAL 0x50

// pre-charge current шаг 25 мА поле GENMASK(5,0)
// 1500 мА = 0x3C (60 * 25мА) чтобы батарея заряжалась даже ниже 3.8 В
// (дефолтные 250-500 мА не покрывают потребление Windows 11 ARM)
#define CHG_PRE_VAL 0x3C

// float voltage шаг 7.5 мВ база 3.4875 В
// 4.390 В = 0x78 как ChgFvMax в конфиге вендора
#define CHG_FLOAT_VAL ((4390000 - 3487500) / 7500)

// разрешенный диапазон температуры для зарядки в 0.1 K
// вышли из диапазона - CHG_EN гасится в том числе во время заряда
#define TEMP_CHG_MIN_K10 2732 // ~ 0 C
#define TEMP_CHG_MAX_K10 3181 // ~ 45 C

/*
    гистерезис температурного окна зарядки в 0.1 K
    гасим на границе TEMP_CHG_MIN/MAX_K10 а включаем заново
    только внутри окна суженного на этот запас с каждой стороны
*/
#define TEMP_CHG_HYST_K10 30 // ~ 3 C

// -=-=-=-=-=-=-=-= фонарик =-=-=-=-=-=-=-=

/*
    оба диода задней вспышки OP6 не RGB-индикатор
    200 мА на диод без safety timer
    1000 мА камерной вспышки сюда не ставить
*/
#define TORCH_MAX_MA      200
#define TORCH_IRES_UA     12500
#define TORCH_MAX_RAW     ((UCHAR)(TORCH_MAX_MA * 1000 / TORCH_IRES_UA)) // 16

#define OP6_TORCH_DOS_DEVICE_NAME L"\\DosDevices\\OP6Torch"

// {7C3E9A12-4B6F-4E81-9D20-A1B2C3D4E5F6}
DEFINE_GUID(GUID_DEVINTERFACE_OP6_TORCH,
    0x7c3e9a12, 0x4b6f, 0x4e81, 0x9d, 0x20, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6);

#define IOCTL_OP6_TORCH_SET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA10, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _OP6_TORCH_REQ {
    UCHAR Brightness;   // 0 = выкл, 1..100
} OP6_TORCH_REQ, *POP6_TORCH_REQ;

// -=-=-=-=-=-=-=-= регистры и маски Flashlight.c =-=-=-=-=-=-=-=

/*
    адреса регистров маски и значения записи для Flashlight.c
    фонарик: блок FLASH LED v2 на слейве SLAVE (2)
    светодиод: каналы LPG и Tri-LED на слейве LED_SLAVE (3)
    номер слейва уходит в старшие 16 бит адреса как в SPMI_ADDR
*/

// SPMI слейв 3: LPG и Tri-LED (зарядник и фонарик на SLAVE 2)
#define LED_SLAVE 3

enum {
    // FLASH LED v2 (SLAVE 2, база 0xD300) фонарик
    FLASH_BASE         = 0xD300,
    FLASH_SAFETY_TIMER = 0xD340,    // бит 7 = таймер; 0 = держать пока не выключим
    FLASH_MAX_CURRENT  = 0xD341,
    FLASH_LED1_CUR     = 0xD342,    // шаг IRES 12.5 мА
    FLASH_LED2_CUR     = 0xD343,    // второй диод той же вспышки
    FLASH_CLAMP_CUR    = 0xD345,
    FLASH_IRES         = 0xD346,    // по 2 бита на диод: 10b = 12.5 мА
    FLASH_STROBE_CFG   = 0xD347,    // 00 = программный строб
    FLASH_STROBE_CTRL  = 0xD348,    // бит 0 LED1 бит 1 LED2

    // значения и маски фонарика
    TORCH_IRES_VAL    = 0x0A,       // FLASH_IRES: LED1 и LED2 по 12.5 мА LSB
    TORCH_STROBE_LEDS = 0x03,       // FLASH_STROBE_CTRL: LED1 | LED2

    // LPG (LED_SLAVE) базы каналов
    LPG_RED_BASE   = 0xB500,        // канал 5 красный
    LPG_GREEN_BASE = 0xB400,        // канал 4 зеленый
    LPG_BLUE_BASE  = 0xB300,        // канал 3 синий

    // LPG регистры канала: смещение от базы канала
    LPG_PWM_SIZE_CLK   = 0x41,      // размер ШИМ и тактирование
    LPG_PWM_PREDIV_CLK = 0x42,      // предделитель
    LPG_PWM_TYPE_CTL   = 0x43,      // конфигурация типа
    LPG_PWM_VALUE_LSB  = 0x44,      // скважность биты 0..7
    LPG_PWM_VALUE_MSB  = 0x45,      // скважность бит 8 (размер 9 бит)
    LPG_PWM_ENABLE_CTL = 0x46,      // включение канала
    LPG_PWM_SYNC       = 0x47,      // синхронизация записи

    // значения записи LPG
    LPG_VAL_SIZE_CLK = 0x12,        // 0x41: 32 кГц и 9 бит -> ШИМ 64 Гц ровный свет
    LPG_VAL_PREDIV   = 0x00,        // 0x42: div 1 exp 1
    LPG_VAL_TYPE     = 0x00,        // 0x43: конфигурация типа
    LPG_VAL_EN_ON    = 0xA4,        // 0x46: 0x80 | 0x20 | 0x04 канал включен
    LPG_VAL_EN_OFF   = 0x20,        // 0x46: канал погашен бит 5 остается
    LPG_VAL_SYNC     = 0x01,        // 0x47: защелкнуть новые значения

    // скважность ШИМ 9 бит лежит в двух регистрах
    LPG_DUTY_MAX      = 511,        // верх скважности 9 бит
    LPG_DUTY_LSB_MASK = 0xFF,       // биты 0..7 в LPG_PWM_VALUE_LSB
    LPG_DUTY_MSB_MASK = 0x01,       // бит 8 в LPG_PWM_VALUE_MSB

    // Tri-LED (LED_SLAVE) подключает каналы LPG к питанию
    TRI_LED_SRC = 0xD045,           // источник питания каналов
    TRI_LED_EN  = 0xD046,           // включение каналов

    // значения и маски Tri-LED
    TRI_LED_SRC_VPH_PWR = 0x01,     // TRI_LED_SRC: питание VPH_PWR
    TRI_LED_EN_RED   = 0x80,        // TRI_LED_EN бит 7: красный (LPG 5)
    TRI_LED_EN_GREEN = 0x40,        // TRI_LED_EN бит 6: зеленый (LPG 4)
    TRI_LED_EN_BLUE  = 0x20,        // TRI_LED_EN бит 5: синий (LPG 3)
};

// пороги автоцвета RGB-индикатора
// кабель есть: красный ниже LED_SOC_RED желтый ниже LED_SOC_GREEN иначе зеленый
// кабеля нет: выкл
#define LED_SOC_RED   20
#define LED_SOC_GREEN 90

// -=-=-=-=-=-=-=-= регистры PMI8998 =-=-=-=-=-=-=-=

// адреса фиксированы железом которые менять нельзя

enum {
    // REVID (база 0x0100) ревизия кремния
    // REVISION4 блока REVID (база 0x0100) старшая ревизия
    // меньше PMI8998_V2P0_REV4 - это v1 у него переставлены байты VBAT/IBAT
    REVID_REV4 = 0x0103,
    PMI8998_V2P0_REV4 = 0x02,

    // CHGR (база 0x1000)
    CHG_STS = 0x1006,               // BATTERY_CHARGER_STATUS_1: [2:0] состояние зарядки
                                    // 3 = FULLON (заряд идет) 7 = DISABLE (сброс watchdog или нет входного питания)
                                    // поле chg в diag: 47 это state 7 плюс бит 6, 03 это state 3
    CHG_EN = 0x1042,                // CHARGING_ENABLE_CMD бит 0
    CHG_FAST = 0x1043,              // ALLOW_FAST_CHARGING_CMD: в unlock пишется MASK_EN=1
    CHG_CFG2 = 0x1051,              // CHGR_CFG2: источник и полярность CHG_EN
    CHG_PRE = 0x1060,
    CHG_FCC = 0x1061,
    CHG_FLOAT = 0x1070,

    // DCDC (база 0x1100) входной тракт и лимит тока
    ICL_MAX_STS = 0x1106,           // ICL_MAX_STATUS поле icl6 в diag
    ICL_STS = 0x1107,               // ICL_STATUS поле icl7 в diag
    PWR_PATH_STS = 0x110B,          // POWER_PATH_STATUS поле pps в diag на PMI8998 читается нулем

    // USBIN (база 0x1300) детектор источника и вход
    USB_IN_STATUS = 0x1306,         // SCHG_USB_STATUS (bits 0..5: 5V..12V input)
    APSD_STS = 0x1307,              // APSD_STATUS: бит 0 = детект завершен бит 5 = SLOW_PLUGIN_TIMEOUT
    APSD_RES = 0x1308,              // APSD_RESULT_STATUS: SDP.0 CDP.2 DCP.3 FLOAT.4 QC2.5 QC3.6
    USB_STS = 0x1310,               // USBIN INT_RT_STS (USBIN_BASE + INT_RT_STS_OFFSET)
    USB_SUSP = 0x1340,              // USBIN_CMD_IL: бит 0 = suspend входного тракта (поле lim в diag)
    USB_APSD = 0x1341,              // CMD_APSD: бит 0 = APSD_RERUN (бывший USB_OVR по Task 24 это rerun)
    USB_FLOAT = 0x1363,             // USBIN_OPTIONS_2_CFG: [2:0] поведение при FLOAT-детекте
                                    // бит 0 FORCE_FLOAT_SDP бит 1 SUSPEND_FLOAT бит 2 FLOAT_DIS_CHGING
    USB_LOAD = 0x1365,              // USBIN_LOAD_CFG: бит 4 = ICL_OVERRIDE_AFTER_APSD (поле ovr65 в diag)
    USB_OPT = 0x1366,               // USBIN_ICL_OPTIONS: бит 0 MODE_CHG бит 1 USB51 бит 2 USB3P0 (поле opt)
    USB_ICL = 0x1370,

    // MISC (база 0x1600) watchdog зарядника
    CHG_WD_PET = 0x1643,            // BARK_BITE_WDOG_PET: запись 0x01 сбрасывает watchdog зарядника
                                    // выполняется прямой записью байта, без read-modify-write
    CHG_WD_SNARL = 0x1653,          // SNARL_BARK_BITE_WD_CFG

    // FG (база 0x4000) топливомер
    // PERPH_SUBTYPE (смещение 0x05) блоков fuel gauge
    // по ним один раз проверяем что адресация SPMI попадает именно в PMI8998
    FG_SOC_SUBTYPE = 0x4005,        // PERPH_SUBTYPE блока BATT_SOC
    FG_SOC = 0x4009,
    FG_INFO_SUBTYPE = 0x4105,       // PERPH_SUBTYPE блока BATT_INFO
    FG_VBAT = 0x41A0,
    FG_IBAT = 0x41A2,

    // ожидаемые значения PERPH_SUBTYPE для hw_identify (не адреса)
    FG_SOC_SUBTYPE_VAL = 0x10,      // FG_BATT_SOC_PMI8998
    FG_INFO_SUBTYPE_VAL = 0x11,     // FG_BATT_INFO_PMI8998

};

// -=-=-=-=-=-=-=-= биты регистров =-=-=-=-=-=-=-=

#ifndef BIT
#define BIT(n)          ((UCHAR)(1u << (n)))
#endif
#ifndef GENMASK
#define GENMASK(h, l)   ((UCHAR)(((1u << ((h) + 1)) - 1) & ~((1u << (l)) - 1)))
#endif

// CHGR включение и состояние заряда
#define CHARGING_ENABLE_CMD_BIT     BIT(0)        // 0x1042.0 зарядка включена
#define ALLOW_FAST_CHARGING_BIT     BIT(0)        // 0x1043.0 быстрая зарядка разрешена
#define CHG_STS_MASK                GENMASK(2, 0) // 0x1006[2:0] состояние заряда

// состояние заряда 0x1006[2:0]
// 0 INHIBIT 1 TRICKLE 2 PRE 3 FULLON 4 TAPER 5 TERMINATE 6 PAUSE
enum {
    DISABLE_CHARGE = 7,            // выключен - лечится фронтом CHG_EN 0 -> 1
};

// USBIN вход и детектор источника
#define USBIN_SUSPEND_BIT           BIT(0)        // 0x1340.0 suspend входного тракта
#define APSD_DTC_STATUS_DONE_BIT    BIT(0)        // 0x1307.0 детектор источника закончил
#define FORCE_FLOAT_SDP_CFG_BIT     BIT(0)        // 0x1363.0 FLOAT -> SDP
#define ICL_OVERRIDE_AFTER_APSD_BIT BIT(4)        // 0x1365.4 ICL из 0x1370 после детекта
#define USBIN_MODE_CHG_BIT          BIT(0)        // 0x1366.0 зарядка от входа

// -=-=-=-=-=-=-=-= маски битовых полей =-=-=-=-=-=-=-=

enum {
    MASK_NONE = 0x00,               // пишем целиком
    MASK_EN   = 0x01,               // младший бит enable/disable (CHG_EN, CHG_FAST, USB_SUSP)
    MASK_PRE = 0x3F,                // PRE_CHARGE_CURRENT_SETTING GENMASK(5,0) биты 6-7 не трогаем
    MASK_OPT3 = 0x07,               // младшие 3 бита: USB_OPT (0x1366) и USB_FLOAT (0x1363)
    MASK_OVR  = 0x10,               // USB_LOAD (0x1365) бит 4 ICL_OVERRIDE_AFTER_APSD

    // CHGR_CFG2: CHG_EN_SRC (бит 7) | CHG_EN_POLARITY (бит 6)
    //
    // 00 = программный источник (регистр CHARGING_ENABLE_CMD) active high
    // 1x = аппаратный источник (внешний GPIO пин)
    //
    // запись 0 под маской 0xC0 принудительно переводит PMIC в Software Control
    //
    // Без этого PMIC игнорирует регистр CHG_EN ожидая сигнала и на физическом пине зарядка не включится
    // (Windows-драйвер не управляет этим GPIO)
    MASK_CFG2_EN = 0xC0,
};

// -=-=-=-=-=-=-=-= таблица записи SPMI =-=-=-=-=-=-=-=

/*
    одна строка записи: адрес регистра + маска + значение
    mask = MASK_NONE значит писать байт целиком
    val уже сдвинуто в позицию поля под маской
*/
typedef struct _SPMI_WRITE {
    USHORT reg;
    UCHAR  mask;
    UCHAR  val;
} SPMI_WRITE;

// -=-=-=-=-=-=-=-= mailbox =-=-=-=-=-=-=-=

// физический адрес разделяемой памяти с данными загрузчика
// там лежит структура MB с последним известным состоянием
#define MB_PA 0x9FFE1000ULL

// сигнатура в начале MB
// "BATT" в little-endian по ней проверяем что структура валидна
#define MB_SIG 0x54544142u

// -=-=-=-=-=-=-=-= SPMI интерфейс =-=-=-=-=-=-=-=

// сигнатуры колбэков интерфейса Qualcomm SPMI Bus
typedef NTSTATUS(*PFN_R)(PVOID, ULONG, ULONG, PUCHAR, ULONG, PULONG);
typedef NTSTATUS(*PFN_W)(PVOID, ULONG, ULONG, PUCHAR, ULONG);
typedef VOID(*PFN_D)(PVOID);

#pragma pack(push, 1)
typedef struct {
    UINT32 Sig, Ver, Mv;
    UINT8  Present, Chg, Proto, Soc, Spmi, ChgSt;
    UINT16 RawV;
    UINT8  RawSoc, T0, T1, T2, T3, Pad[3];
} MB;
#pragma pack(pop)

#pragma pack(push, 8)
typedef struct {
    USHORT                 Size, Version;
    PVOID                  Context;
    PINTERFACE_REFERENCE   Ref;
    PINTERFACE_DEREFERENCE Deref;
    PVOID                  SpmiContext;
    PFN_R                  ReadLong;
    PFN_W                  WriteLong;
    PVOID                  Reserved[7];
} QCOM_SPMI;
#pragma pack(pop)

// -=-=-=-=-=-=-=-= снимок состояния =-=-=-=-=-=-=-=

/*
    состояние батареи один тип на две копии в контексте:
    hw - рабочая ее пишет только поток опроса
    snap - опубликованная под SnapLock для читателей (колбэки batclass)
*/
typedef struct _OP6_SNAP {
    ULONG   Volt;       // напряжение в мВ
    ULONG   Soc;        // заряд в процентах 0..100
    ULONG   Cap;        // остаток в мА·ч из bq27411 (RC)
    ULONG   Temp;       // температура в 0.1 K 0 = не прочитана
    LONG    Curr;       // ток в мА отрицательный при разряде 0 = не измерен
    LONG    Rate;       // ток в мА (относительные единицы как Cap) или BATTERY_UNKNOWN_RATE
    BOOLEAN Ac;         // TRUE если есть внешнее питание сбрасывается в начале опроса
    BOOLEAN Charging;   // TRUE если идет активный заряд
    BOOLEAN Valid;      // TRUE после первого успешного чтения SOC+VBAT
    BOOLEAN TempFresh;  // TRUE если Temp прочитана и валидна в этом опросе
    BOOLEAN AcFresh;    // TRUE если Ac прочитан из USB_STS (или mailbox) в этом опросе
    BOOLEAN Crit;       // TRUE если критический заряд подтвержден CRIT_CONFIRM опросами
} OP6_SNAP;

// -=-=-=-=-=-=-=-= контекст =-=-=-=-=-=-=-=

typedef struct _OP6_CTX {
    WDFDEVICE   Device;         // WDF-объект устройства
    WDFTIMER    Timer;          // опрос железа one-shot с перевзводом
    PVOID       Class;          // хэндл batclass заполняет BattAttach
    WDFWAITLOCK ClassLock;      // Class против BattAttach/BattDetach (EvtIoctl)
    WDFSPINLOCK SnapLock;       // Snap/Notify против потока опроса
    OP6_SNAP    Hw;             // рабочее состояние: read_hw BattCommit
    OP6_SNAP    Snap;           // опубликованная копия: пишет BattCommit

    ULONG       Tag;            // батарея несъемная: EvtAdd ставит 1
    WDFIOTARGET Target;         // io target на \Device\Spmi
    WDFIOTARGET I2cTarget;      // io target на bq27411 (I2C) или NULL

    QCOM_SPMI   Spmi;           // Провайдер удерживает адрес интерфейса до Deref

    // интерфейс SPMI заполняется при первом успешном open
    PVOID       SpmiBus;        // контекст шины от qcspmi8480.sys
    PFN_R       SpmiRead;       // чтение регистров
    PFN_W       SpmiWrite;      // запись регистров
    PFN_D       SpmiDeref;      // освобождение интерфейса
    PVOID       SpmiIface;      // контекст интерфейса для SpmiDeref
    ULONG       SpmiRetry;      // опросов до следующей попытки открыть SPMI
    ULONG       SpmiBackoff;    // текущая пауза между попытками удваивается
    ULONG       SpmiFail;       // подряд неудачных опросов (нет VBAT или SOC и нет mailbox)

    PVOID       Mailbox;        // замапленный регион MB_PA или NULL

    // емкость заполняет read_capacity из bq27411
    // значения почти статика и читатели берут их напрямую без лока
    // единицы мАч родные для gauge в мВтч не переводим как в Linux
    // Full == 0 значит еще не прочитана
    ULONG       Full;           // полная емкость мАч (FCC 0x0E bq27411)
    ULONG       Design;         // паспортная емкость мАч (DCAP 0x3C bq27411)

    // флаги потока опроса
    BOOLEAN     Fast;           // TRUE если зарядник уже настроен
    BOOLEAN     Run;            // TRUE пока таймер должен перевзводиться
    BOOLEAN     ChgOff;         // CHG_EN погашен нами при выходе вернуть
    BOOLEAN     HwOk;           // FG опознан как PMI8998 (PERPH_SUBTYPE)
    BOOLEAN     Rev1Wa;         // PMI8998 v1: байты VBAT/IBAT переставлены (PMI8998_V1_REV_WA)
    BOOLEAN     TorchOn;        // строб сейчас горит выключить в HwExit
    ULONG       CritCnt;        // подряд опросов с критическим признаком пишет BattCommit

    // пороги уведомлений от batclass пишет QNotify читает EvtTimer
    BATTERY_NOTIFY Notify;
    BOOLEAN     NotifyValid;
    BOOLEAN     LowFlag, HighFlag;      // прошлые состояния порогов
    BOOLEAN     PrevAc, PrevChg;        // прошлое состояние для детекта смены
    BOOLEAN     PrevValid;              // прошлое Hw.Valid
    BOOLEAN     PrevCrit;               // прошлое состояние "критический заряд"

    // Управление встроенным RGB-светодиодом
    UCHAR       LastLedState;           // 0 = OFF, 1 = RED, 2 = YELLOW, 3 = GREEN, 0xFF = uninit
    BOOLEAN     LedManualOverride;      // TRUE если цвет задан пользователем через IOCTL
} OP6_CTX, * POP6_CTX;

// -=-=-=-=-=-=-=-= интерфейс =-=-=-=-=-=-=-=

// регистрация в batclass.sys вызывается из EvtSmInit
NTSTATUS BattAttach(POP6_CTX c);

// снятие регистрации вызывается из EvtSmCleanup
VOID BattDetach(POP6_CTX c);

// сведение состояния и публикация снимка вызывается из HwPoll
VOID BattCommit(POP6_CTX c);

// инициализация железа вызывается из EvtSmInit
VOID HwInit(POP6_CTX c);

// освобождение железа вызывается из EvtSmCleanup
VOID HwExit(POP6_CTX c);

// периодический опрос вызывается из таймера и в конце HwInit
VOID HwPoll(POP6_CTX c);

// вернуть CHG_EN если погасили мы, зовется из HwExit пока шина еще жива
VOID HwRestoreCharge(POP6_CTX c);

// опознание PMI8998 по PERPH_SUBTYPE, зовется из ensure_spmi после Open
BOOLEAN hw_identify(POP6_CTX c);

// открыть SPMI если еще не открыт (WDF target + QueryForInterface)
VOID ensure_spmi(POP6_CTX c);

// закрыть канал и отдать интерфейс, объект target живет до HwExit
VOID SpmiTeardown(POP6_CTX c, BOOLEAN is_open);

// снять маппинг mailbox загрузчика
VOID mailbox_unmap(POP6_CTX c);

// чтение 16-битного регистра bq27411 по I2C, таргет из PrepareHardware
BOOLEAN i2c_rd(POP6_CTX c, UCHAR reg, PULONG v);

// фонарик: 0 = выкл, 1..100 = яркость. шину открывает сам
NTSTATUS TorchSet(POP6_CTX c, UCHAR brightness);

// погасить без открытия шины: нет SpmiWrite - тихий выход
VOID TorchOff(POP6_CTX c);

// строка в \SystemRoot\op6_debug.log формат как у printf только PASSIVE_LEVEL
VOID DbgLog(PCSTR format, ...);

// RGB-светодиод уведомлений: компоненты 0-255, (0,0,0) = выкл
VOID LedSetRgb(POP6_CTX c, UCHAR r, UCHAR g, UCHAR b);

// автоцвет по батарее: RED<20 YELLOW<90 GREEN>=90 при AC, без AC выкл; зовет HwPoll
VOID LedUpdateBattery(POP6_CTX c);

#endif /* OP6BATTERY_H */
