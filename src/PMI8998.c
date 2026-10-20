#include <ntddk.h>
#include <wdf.h>

#include "Op6Battery.h"

// -=-=-=-=-=-=-=-= преобразования единиц =-=-=-=-=-=-=-=

/*
    регистры PMIC отдают сырые числа
    чтобы получить нормальные единицы число множим на цену одного шага

    функции ниже переводят raw в единицы
    цены шагов взяты из драйвера qpnp-fg-gen3 менять их нельзя
*/

/*
    напряжение raw в милливольты
    один шаг датчика 122.07 мкВ (122070 нВ) делим на 1000000 чтобы получить мВ
    пример raw около 28670 дает около 3500 мВ

    большие raw не влезают в 32 бита
    поэтому считаем в 64 битах и только потом возвращаем 32
*/
static ULONG to_mv(USHORT raw)
{
    return (ULONG)(((ULONGLONG)raw * FG_VBAT_LSB_NV) / UNIT_NANO_DIV);
}

/*
    ток = raw -> миллиамперы
    ЗНАК ПЕРЕВЕРНУТ специально

    датчик считает заряд отрицательным числом
*/
static LONG to_ma(SHORT raw)
{
    return -(LONG)(((LONGLONG)raw * FG_IBAT_LSB_NA) / (LONGLONG)UNIT_NANO_DIV);
}

/*
    заряд батареи берем из двух соседних регистров
*/
static ULONG to_soc(UCHAR a, UCHAR b)
{
    UCHAR v = a;

    if (!v) return 0;

    // b == 0 считаем отсутствием данных
    // иначе min дал бы 0%
    if (b && b < v) v = b;

    // 0xFF не сырое значение а признак полной батареи
    if (v == SOC_RAW_FULL) return SOC_PCT_FULL;

    /*
        Сырой диапазон 1..254 растягиваем в 1..99
        0xFF выше уже дал 100
        формула из mainline с округлением к ближайшему как в Linux
        чтоб не срезать остаток и не занижать на границах
    */
    return ((ULONG)(v - SOC_RAW_MIN) * SOC_SPREAD + SOC_ROUND) / SOC_DIV + SOC_RAW_MIN;
}

/*
    на PMI8998 v1 байты VBAT и IBAT переставлены (аппаратный баг)
    на v2 raw идет как есть

    пример пришло 0x1234 а на самом деле 0x3412
    старший и младший байты поменялись местами
    RtlUshortByteSwap меняет их обратно

    включается флагом c->Rev1Wa
    флаг ставит hw_identify по ревизии кремния
    на наших телефонах rev1wa=0 значит обход не нужен
*/
static USHORT fg_fix(POP6_CTX c, USHORT raw)
{
    return c->Rev1Wa ? RtlUshortByteSwap(raw) : raw;
}

// -=-=-=-=-=-=-=-= доступ к шине =-=-=-=-=-=-=-=

/*
    прочитать n байт из регистра reg в буфер buf
    самая нижняя "кирпичная" функция
    все чтения PMIC в этом файле идут только через нее

    адрес для шины собирает макрос SPMI_ADDR
    старший байт это номер слейва (у PMI8998 он 2)
    младшие два байта это адрес регистра
    c->SpmiRead это указатель на функцию ReadLong которую дал шинный драйвер
    пока шина не открыта (см ensure_spmi) там NULL и читать нельзя
    второй аргумент вызова у нас всегда 0 мы его не используем

    возвращает TRUE только если прочитали ровно n байт
*/
static BOOLEAN spmi_rd(POP6_CTX c, USHORT reg, PUCHAR buf, ULONG n)
{
    ULONG got = 0;
    NTSTATUS st;

    if (!c->SpmiRead) return FALSE;

    // без частичного мусора в буфере вызывающего
    RtlZeroMemory(buf, n);
    st = c->SpmiRead(c->SpmiBus, 0, SPMI_ADDR(reg), buf, n, &got);

    /*
        тут только st == SPMI_SUCCESS а не NT_SUCCESS
        qcspmi возвращает коды шины SPMI где ошибки положительные

        SPMI_SUCCESS = 0 Успех
        5 FAILURE
        6 DENIED
        7 DROPPED
        12 неверный контекст

        NT_SUCCESS считает успехом любое >= 0 и пропустил бы их

        при чтении спасал got == n а при записи второй проверки не было
        команда CHG_EN=0 при перегреве не проходила а драйвер считал что все ок

        для WDF/DDI по прежнему NT_SUCCESS там ошибки отрицательные
    */
    return (BOOLEAN)(st == SPMI_SUCCESS && got == n);
}

/*
    читаем 16-битное число из двух соседних регистров reg и reg+1
    формат little-endian младший байт по адресу reg старший по reg+1
    пример b[0]=0x34 и b[1]=0x12 дает 0x1234

    сначала пробуем взять оба байта одной транзакцией так быстрее
    если шина так не умеет или отказала читаем по одному байту
    если не вышло и так возвращаем ошибку а *v не трогаем
*/
static NTSTATUS rdw(POP6_CTX c, USHORT reg, PUSHORT v)
{
    UCHAR b[2];

    // SPMI на 2 байта может не пройти читаем по одному
    if (!spmi_rd(c, reg, b, 2)) {
        if (!spmi_rd(c, reg, &b[0], 1) ||
            !spmi_rd(c, (USHORT)(reg + 1), &b[1], 1))
            return STATUS_DEVICE_DATA_ERROR;
    }

    *v = MAKE_USHORT_LE(b[0], b[1]);
    return STATUS_SUCCESS;
}

/*
    одноразовая идентификация топливомера по PERPH_SUBTYPE

    зачем это вообще нужно
    адрес на шине это просто число и если слейв или адрес перепутаны
    мы будем читать а хуже того писать совсем другую микросхему
    поэтому прежде чем что то писать убеждаемся что говорим именно с PMI8998
    у каждого блока внутри PMIC есть регистр PERPH_SUBTYPE с номером модели
    у нашего топливомера значения известны заранее (0x10 у SOC и 0x11 у INFO)

    пока HwOk == FALSE функция wrb ничего не пишет
    на чтение батареи HwOk не влияет читаем и без опознания

    вызывается из ensure_spmi сразу после открытия шины
    если опознать не вышло следующая попытка только при следующем открытии шины
*/
BOOLEAN hw_identify(POP6_CTX c)
{
    UCHAR a, b, r;

    // уже опознали раньше повторять не надо
    if (c->HwOk) return TRUE;

    if (!spmi_rd(c, FG_SOC_SUBTYPE, &a, 1) ||
        !spmi_rd(c, FG_INFO_SUBTYPE, &b, 1))
        return FALSE;

    DbgLog("hw_identify: 0x4005=0x%02X 0x4105=0x%02X", a, b);
    c->HwOk = (BOOLEAN)(a == FG_SOC_SUBTYPE_VAL && b == FG_INFO_SUBTYPE_VAL);

    // Если шина доступна то продолжаем
    if (!c->HwOk) return FALSE;

    /*
        ревизия кремния нужна чтобы понимать какая версия чипа перед нами
        у первой версии (v1) байты VBAT и IBAT переставлены (см fg_fix)
        v2 это серийная версия там все как есть
        читаем один раз вместе с идентификацией
        не прочиталась или 0 считаем серийной и обход не включаем
    */
    c->Rev1Wa = (BOOLEAN)(spmi_rd(c, REVID_REV4, &r, 1) &&
        r && r < PMI8998_V2P0_REV4);

    DbgLog("hw_identify: 0x0103=0x%02X rev1wa=%u", r, c->Rev1Wa);

    return TRUE;
}

/*
    запись регистра
    mask == 0 пишем целиком иначе read-modify-write
    единая точка записи сама проверяет интерфейс и опознанное железо

    зачем нужна mask
    в регистре обычно соседствуют чужие биты которые трогать нельзя
    пример надо поменять младшие 6 бит (0x3F) а два старших оставить
      1) читаем текущее    r = 0xC5 (11000101)
      2) обнуляем под маской r & ~0x3F = 0xC0
      3) вписываем наше    | (val & 0x3F)
      4) пишем обратно
    это и есть read-modify-write
    mask == 0 значит чужих бит нет читать не нужно

    возвращает TRUE если запись прошла
*/
static BOOLEAN wrb(POP6_CTX c, USHORT reg, UCHAR mask, UCHAR val)
{
    UCHAR r = val;
    NTSTATUS st;

    // писать можно только когда есть интерфейс и железо опознано
    if (!c->SpmiWrite || !c->HwOk) return FALSE;

    if (mask) {
        if (!spmi_rd(c, reg, &r, 1)) return FALSE;
        r = (r & ~mask) | (val & mask);
    }

    st = c->SpmiWrite(c->SpmiBus, 0, SPMI_ADDR(reg), &r, 1);

    /*
        успех только st == SPMI_SUCCESS тот же контракт что в spmi_rd
        раньше !NT_SUCCESS пропускал положительные коды ошибок шины
        и запись считалась успешной не исполнившись

        Fast вставал в 1 а команда не доходила до PMIC
        термозащитная запись CHG_EN=0 с таким кодом
        гасила заряд только в воображении драйвера
    */
    if (st != SPMI_SUCCESS) {
        DbgLog("wrb failed reg=0x%04X st=0x%08X", reg, st);
        return FALSE;
    }
    return TRUE;
}

// -=-=-=-=-=-=-=-= настройка зарядника =-=-=-=-=-=-=-=

/*
    можно сейчас заряжать или нет

    окно зарядки бывает двух размеров:
      hyst = FALSE широкое: пока температура внутри зарядку держим
                            вылетели из широкого сразу гасим

      hyst = TRUE  узкое:   зарядку ВКЛЮЧАТЬ можно только из него

    узкое сидит внутри широкого это не баг а гистерезис
    без него на границе зарядка щелкала бы вкл/выкл каждый опрос
    выключили на 45 C и обратно включим только когда реально остыло

    TempFresh = температура из ЭТОГО опроса (сбрасывается в read_hw)
    прочитать не смогли значит летим вслепую
    вслепую зарядку не включаем честный FALSE
*/
static BOOLEAN temp_in(POP6_CTX c, BOOLEAN hyst)
{
    ULONG lo = TEMP_CHG_MIN_K10;
    ULONG hi = TEMP_CHG_MAX_K10;

    if (hyst) {
        lo += TEMP_CHG_HYST_K10;
        hi -= TEMP_CHG_HYST_K10;
    }

    // возвращаем свежую температуру и в диапазоне
    return (BOOLEAN)(c->Hw.TempFresh && IN_RANGE(c->Hw.Temp, lo, hi));
}

// порядок строк = оригинал/XBL разрешение до значения resume последним
//
// CHG_CFG2 первой строкой переводит источник включения заряда в software
// без этого PMIC слушает физический пин CHG_EN на OP6 притянут к земле
// и записи 0x1042 игнорируются заряд не включить и не погасить
// так же делает init mainline smb2-charger CHG_EN_SRC|POLARITY = 0
static const SPMI_WRITE chg_seq[] = {
    { CHG_CFG2,  MASK_CFG2_EN, 0 },                        // Software Control CHG_EN из регистра не с пина
    { CHG_FLOAT, MASK_NONE, CHG_FLOAT_VAL },               // предел напряжения 4.390 В
    { CHG_PRE,   MASK_PRE,  CHG_PRE_VAL   },               // ток предзаряда 1500 мА
    { CHG_FCC,   MASK_NONE, FCC_REG       },               // ток зарядки 2000 мА
    { USB_FLOAT, MASK_OPT3, FORCE_FLOAT_SDP_CFG_BIT },     // FLOAT в SDP вход не suspend-ится
    { USB_LOAD,  MASK_OVR,  ICL_OVERRIDE_AFTER_APSD_BIT }, // override лимита после APSD
    { USB_OPT,   MASK_OPT3, USBIN_MODE_CHG_BIT },          // MODE_CHG
    { USB_ICL,   MASK_NONE, ICL_REG_VAL   },               // лимит входного тока
    { USB_SUSP,  USBIN_SUSPEND_BIT, 0 },                   // снять suspend входа последним
    { CHG_FAST,  MASK_EN,   ALLOW_FAST_CHARGING_BIT },     // разрешить быструю зарядку
};

/*
    unlock настраивает и включает зарядник
    зовется из HwPoll при внешнем питании
    срабатывает один раз
    после успешной настройки ставит c->Fast и дальше сразу выходит
*/
static VOID unlock(POP6_CTX c)
{
    ULONG i;
    UCHAR apsd, chg;

    // нет телеметрии или свежей температуры или записи по шине значит зарядник не трогаем
    if (c->Fast || !c->Hw.Valid || !c->SpmiWrite) return;

    if (!temp_in(c, TRUE)) return;

    // конфиг тракта
    for (i = 0; i < RTL_NUMBER_OF(chg_seq); i++)
        if (!wrb(c, chg_seq[i].reg, chg_seq[i].mask, chg_seq[i].val))
            return;

    // включаем заряд только после конца детектора источника
    if (!spmi_rd(c, APSD_STS, &apsd, 1) || !(apsd & APSD_DTC_STATUS_DONE_BIT) ||
        !spmi_rd(c, CHG_STS, &chg, 1))
        return;

    // state 7 значит DISABLE
    if ((chg & CHG_STS_MASK) == DISABLE_CHARGE && !wrb(c, CHG_EN, MASK_EN, 0))
        return;

    if (!wrb(c, CHG_EN, MASK_EN, CHARGING_ENABLE_CMD_BIT))
        return;

    c->Fast   = TRUE;
    c->ChgOff = FALSE;
}

// -=-=-=-=-=-=-=-= шаги чтения =-=-=-=-=-=-=-=

/*
    напряжение обязательно
    если не читается значит с шиной или железом беда
    и остальные регистры в этом опросе читать уже бессмысленно

    конвейер rdw -> fg_fix (порядок байтов для v1) -> to_mv
    значение принимаем как есть сырым без проверки диапазона
    мусор теперь виден в диагностике (vraw и vbat в строке diag)
    а не отброшен молча вместе со всем опросом
    возвращаем TRUE только если чтение с шины прошло
*/
static BOOLEAN read_vbat(POP6_CTX c)
{
    USHORT raw = 0;

    if (!NT_SUCCESS(rdw(c, FG_VBAT, &raw))) return FALSE;

    c->Hw.Volt = to_mv(fg_fix(c, raw));
    return TRUE;
}

/*
    наличие кабеля по двум регистрам, достаточно любого из них
    USBIN_INPUT_STATUS 0x1306 bits 0..5 (5V..12V) аппаратный детектор VBUS
    USBIN_PLUGIN_RT_STS (бит 4 маска 0x10 в USB_STS) plugin-бит

    на OP6 plugin-бит часто молчит при живом VBUS (роль USB при буте)
    а 0x1306 при живом VBUS уже ненулевой, поэтому он добавлен в проверку

    Ac = TRUE если сработал хотя бы один регистр
    пишем прямо в Hw и только поднимаем (|=) стартовое FALSE дал read_hw

    при неудаче обоих чтений Ac не восстанавливаем
    read_hw сбросил его в начале опроса
    старый TRUE не должен пережить отключение кабеля

    AcFresh ставится только если хотя бы одно чтение удалось
    иначе read_ibat и BattCommit понимают что флагу доверять нельзя
    и пробуют угадать кабель по току
*/
static VOID read_usb(POP6_CTX c)
{
    UCHAR v;

    // 0x1306 (USBIN_INPUT_STATUS)
    // биты 0..5 (5V..12V) аппаратный детектор VBUS
    if (spmi_rd(c, USB_IN_STATUS, &v, 1)) {
        c->Hw.AcFresh = TRUE;
        if (v & USB_IN_VOLT_MASK)
            c->Hw.Ac = TRUE;
    }

    // 0x1310 (USB_STS)
    // бит 4 (USBIN_PLUGIN_RT_STS) дополнительный статус
    if (spmi_rd(c, USB_STS, &v, 1)) {
        c->Hw.AcFresh = TRUE;
        if (v & USB_STS_PRESENT_MASK)
            c->Hw.Ac = TRUE;
    }
}

/*
    ток батареи в c->Hw.Curr
    после to_ma больше нуля заряд меньше нуля разряд
    отрицательный IBAT это признак активного заряда

    сырой регистр берем как есть без фильтра raw == 0
    молчащий датчик и настоящий ноль теперь не различаем
    на расчеты это не влияет мощность при нуле BATTERY_UNKNOWN_RATE (см BattCommit)

    вторая задача запасной способ узнать про кабель
    если USB_STS в этом опросе не прочитан (AcFresh == FALSE)
    а сырой ток ниже IBAT_RAW_CHG_THRESH значит батарея явно заряжается и кабель есть
    если USB_STS прочитан ему верим больше и эвристику не применяем
*/
static VOID read_ibat(POP6_CTX c)
{
    USHORT raw = 0;
    SHORT  ma;

    if (!NT_SUCCESS(rdw(c, FG_IBAT, &raw))) return;

    ma = (SHORT)fg_fix(c, raw);
    c->Hw.Curr = to_ma(ma);

    if (!c->Hw.AcFresh)
        c->Hw.Ac |= (BOOLEAN)(ma < IBAT_RAW_CHG_THRESH);
}

/*
    SOC читаем до совпадения байтов но не больше SOC_TRIES раз
    TRUE только если Hw.Soc реально обновлен

    FG_SOC и FG_SOC+1 это две копии одного значения
    совпали значит стабильное и можно верить
    не совпали значит топливомер как раз обновляет число
    и мы поймали его посередине - 0x2A 0x2B читаем заново 0x2B 0x2B
    не совпали за все попытки значение не принимаем
    Hw.Soc остается прошлым
*/
static BOOLEAN read_soc(POP6_CTX c)
{
    UCHAR cap[2];
    ULONG tries = SOC_TRIES;

    do {
        if (!spmi_rd(c, FG_SOC, cap, 2)) return FALSE;
    } while (--tries && cap[0] != cap[1]);

    if (cap[0] != cap[1]) return FALSE;

    c->Hw.Soc = to_soc(cap[0], cap[1]);

    return TRUE;
}

/*
    температуру берем из термистора батареи на bq27411 по I2C (регистр 0x02 сразу в 0.1 K)
    FG_TEMP у PMI8998 на этом телефоне не живой
    кристалл и PON не опрашиваем
    основной gauge внешний

    значение берем как есть без проверки диапазона
    мусорная температура вне окна зарядки все равно погасит
    зарядку через temp_in то же самое делал старый фильтр
    TempFresh поднимаем при любом успешном чтении
    если i2c не ответил TempFresh остается FALSE
    а от TempFresh зависит разрешение на зарядку (см temp_in)
*/
static VOID read_temp(POP6_CTX c)
{
    ULONG k10;

    if (!i2c_rd(c, BQ27411_REG_TEMP, &k10)) return;

    c->Hw.Temp      = k10;
    c->Hw.TempFresh = TRUE;
}

/*
    емкость банки из bq27411 по I2C
    PMI8998 выученную емкость регистром не отдает
    она в SRAM QG

    FCC 0x0E выученная полная емкость
    DCAP 0x3C паспортная емкость
    читаем оба один раз до первого успеха и замораживаем

    в мВт·ч: мАч gauge * BATT_NOMINAL_MV / 1000

    ошибка чтения выходим без фолбека
    следующий опрос попробует снова
*/
static VOID read_capacity(POP6_CTX c)
{
    ULONG fcc, dcap;

    // уже принято: значения почти константа перечитывать нечего
    if (c->Full) return;

    if (!i2c_rd(c, BQ27411_REG_FCC,  &fcc))  return;
    if (!i2c_rd(c, BQ27411_REG_DCAP, &dcap)) return;

    // Design раньше Full: читатели ждут пару по признаку Full != 0
    c->Design = dcap * BATT_NOMINAL_MV / 1000;
    c->Full   = fcc * BATT_NOMINAL_MV / 1000;
}

/*
    остаток из bq27411 регистром RC (RemainingCapacity мАч)
    в мВт·ч переводим здесь номиналом как Full в read_capacity

    читаем раз в POLL_MS для published снимка

    при ошибке чтения прошлый остаток живет дальше
    за 2 секунды значение квазистатично (как Hw.Soc у read_soc)
*/
static VOID read_remaining(POP6_CTX c)
{
    ULONG rc;

    // полная емкость не принята - остаток не трогаем
    if (!c->Full) return;

    if (!i2c_rd(c, BQ27411_REG_RC, &rc)) return;

    rc = rc * BATT_NOMINAL_MV / 1000;

    // RC выше Full не бывает
    // Cap/Full клампим
    if (rc > c->Full)
        rc = c->Full;

    c->Hw.Cap = rc;
}

/*
    резервный источник разделяемая память загрузчика

    запасной аэродром если живая шина SPMI молчит
    загрузчик положил в mailbox структура MB см Op6Battery.h
    напряжение проценты и флаг кабель воткнут в момент старта Windows
    данные устаревают сразу поэтому только временная заглушка пока шина не заработает

    TRUE если mailbox существует подписан и в нем есть батарея
    три причины отказаться mailbox не замаплен то есть NULL
    нет подписи MB_SIG значит там лежит не наша структура
    или загрузчик отметил что батареи нет

    поля берем как есть без проверок диапазона
*/
static BOOLEAN read_mb(POP6_CTX c)
{
    volatile MB* m = c->Mailbox;

    if (!m || m->Sig != MB_SIG || !m->Present) return FALSE;

    c->Hw.Volt = m->Mv;
    c->Hw.Soc  = m->Soc;

    /*
        флаг сети берем только если USB в этом опросе молчал
        иначе бутовый Chg затрет свежий VBUS из read_usb

        ветка C достижима при живом чтении VBAT
        vbat прочитан а SOC нет и на своей зарядке покажется разрядом
    */
    if (!c->Hw.AcFresh) {
        c->Hw.Ac = (m->Chg != 0);
        c->Hw.AcFresh = TRUE;
    }

    return TRUE;
}

// -=-=-=-=-=-=-=-= чтение железа =-=-=-=-=-=-=-=

/*
    один опрос батареи от начала до конца

    SPMI - основной источник
    mailbox - запасной

    Valid = TRUE только если в этом опросе прочитали и VBAT и SOC

      1) открыть SPMI если еще не открыта

      2) сбросить ток, кабель и температуру прошлого опроса
         иначе после выдергивания кабеля еще долго будет "заряжается"
         Curr = 0 значит не измерен, Ac = FALSE, TempFresh и AcFresh = FALSE

      3) I2C: температура и емкость с bq27411
         своя шина, SPMI не нужен
         если не прочитать до BattAttach - Windows один раз возьмет Full=0
         и больше не спросит (батарея несъемная, Tag всегда 1)

      4) VBAT с SPMI - не прочитался, остальное с SPMI не трогаем

      5) кабель, ток, SOC

      6) итог
           A) SOC есть      - все ок, Valid = TRUE, mailbox снимаем
           B) SOC нет долго - данные протухли, Valid = FALSE
              батарея несъемная, из системы не исчезает, в UI будет "?"
              шину переоткрываем
           C) SOC нет мало  - берем снимок загрузчика
              держит батарею не дольше SPMI_FAIL_MAX опросов
*/
static VOID read_hw(POP6_CTX c)
{
    BOOLEAN soc = FALSE;
    BOOLEAN ac  = c->Hw.Ac;    // флаг сети прошлого опроса
    BOOLEAN was = c->Hw.Valid; // Valid на входе для диагностики переходов

    ensure_spmi(c);

    // свежесть температуры и флага сети считаем заново каждый опрос
    // ток прошлого опроса не тащим CURR_UNMEASURED значит не измерен
    // ac тоже не переносим через молчащий USB_STS
    c->Hw.TempFresh = FALSE;
    c->Hw.AcFresh   = FALSE;
    c->Hw.Ac        = FALSE;
    c->Hw.Curr      = CURR_UNMEASURED;

    // bq27411 читаем по I2C - в SPMI не лезем
    read_temp(c);
    read_capacity(c);
    read_remaining(c);

    // напряжение читаем первым если оно не читается дальше идти нет смысла
    // soc останется FALSE и попадем в ветку B или C ниже
    if (read_vbat(c)) {
        read_usb(c);
        read_ibat(c);
        soc = read_soc(c);
    } else {
        // шина молчит значит про кабель ничего не знаем
        // не сбрасываем Ac иначе со старым Soc Windows решит что батарея разряжена
        // протухшие данные снимет ветка B
        c->Hw.Ac = ac;
    }

     /*
     батарея рабочая только если прочитали и напряжение и проценты
     если прочитали только напряжение значит железо еще не проснулось
     регистр 0x41A0 уже может отдавать валидные милливольты
     а регистр 0x4009 с процентами еще не готов
     тогда байты процентов не совпадут между собой
     и мы оставим Valid = FALSE чтобы Windows не показывал мусор

     в старой версии драйвера проценты считались по таблице напряжения
     но без аппаратного счетчика кулонов эта оценка сильно врет под нагрузкой
     когда процессор начинает потреблять ток напряжение проседает
     а драйвер думает что батарея села хотя там еще 50 %
     поэтому кривую сюда не переносим и ждем данные из шины

     если SPMI не отвечает данные берем из mailbox
     это память которую загрузчик UEFI оставил перед стартом Windows
     но эти данные не обновляются в реальном времени
     поэтому опрос успешен только если получили свежие данные с шины
     */

    // вариант A живые данные получены
    if (soc) {
        c->SpmiFail = 0;
        c->Hw.Valid = TRUE;
        if (!was) DbgLog("data ok");        // телеметрия вернулась

        /*
            шина SPMI работает значит проценты актуальные
            значит снимок статики из памяти больше не нужен

            если его не удалить то при случайном сбое шины
            драйвер возьмет устаревшие данные из mailbox и откатит проценты назад

            mailbox_unmap зовет функцию ядра MmUnmapIoSpace
            она убирает маппинг физической памяти в виртуалку
        */
        mailbox_unmap(c);
    }

    /*
        вариант B данные протухли

        промах копим всегда когда нет живого SOC раньше не рос пока шина
        не открыта и mailbox замаплен так mailbox переживал INIT_DELAY = 30
        опросов около 60 с но если qcspmi не загрузился и шина ни разу не
        открылась телефон разряжался до нуля без предупреждения бутовые
        проценты не давали ни "?" ни BATTERY_CRITICAL

        Valid = FALSE обязателен иначе QStatus не отдаст BATTERY_UNKNOWN_*
        тег не трогаем батарея несъемная из системы не исчезает

        SpmiTeardown закрывает шину целиком а не сбрасывает указатели
        следующий ensure_spmi откроет с нуля и вылечит зависшую шину

        ++ в условии значит сюда попадаем только если soc == FALSE
    */
    else if (++c->SpmiFail >= SPMI_FAIL_MAX) {

        c->SpmiFail = 0; // чтобы не ушел в бесконечность

        c->Hw.Valid = FALSE;
        if (was) DbgLog("data lost");       // телеметрия потеряна

        // шина мертва значит загрузчик устарел
        mailbox_unmap(c);

        // переоткрываем шину на случай временного зависания
        SpmiTeardown(c, c->SpmiRead != NULL);

        // паузу SpmiRetry тут не взводим
        // поэтому уже следующий опрос сам откроет шину без ожидания
        // если SOC так и не читается шина будет закрываться и открываться
        // каждые SPMI_FAIL_MAX опросов это около 10 секунд
    }

    /*
        вариант C заглушка из mailbox

        срабатывает если SOC нет но промахов еще меньше SPMI_FAIL_MAX
        это окно между стартом драйвера и первым живым опросом
        SPMI_INIT_DELAY или первые backoff-паузы

        живой SOC снимает mailbox ветка A
        SPMI_FAIL_MAX промахов снимает и его и Valid ветка B
        заглушка не вечная по построению максимум ~10 секунд
        после чего ветка B честно переводит батарею в "?"

        поля mailbox берем как есть и Valid поднимаем
        без старого порога вольтажа нулевые мВ раньше гасили Valid
        теперь такое состояние видно в диагностике а не спрятано молча
    */
    else if (read_mb(c))
        c->Hw.Valid = TRUE;
}

/*
    пока SPMI еще открыт если CHG_EN погашен нами возвращаем его
    иначе после выгрузки драйвера зарядка останется выключенной до перезагрузки
*/
VOID HwRestoreCharge(POP6_CTX c)
{
    if (!c->ChgOff)
        return;

    // шина могла уйти target закрыл фреймворк открываем без паузы
    // иначе wrb откажет и CHG_EN останется погашенным
    c->SpmiRetry = 0;
    ensure_spmi(c);

    if (wrb(c, CHG_EN, MASK_EN, CHARGING_ENABLE_CMD_BIT)) {
        c->ChgOff = FALSE;
    } else {
        DbgLog("chg restore failed");
    }
}

// -=-=-=-=-=-=-=-= диагностика зарядника =-=-=-=-=-=-=-=

/*
    дамп зарядника HwPoll зовет каждый опрос
*/
static VOID hw_diag(POP6_CTX c)
{
    // читаем те же регистры что писал unlock
    // порядок значений = порядок regs[] и групп
    // 
    // 17 статусных | vflt pre fcc afst | vraw iraw cap cap
    // | vbat soc curr ac temp | valid fail fast chgoff
    // | bq fcc dcap rc full design hwcap
    static const USHORT regs[] = {
        USB_LOAD,     USB_OPT,       USB_FLOAT, USB_APSD, USB_ICL,
        ICL_MAX_STS,  ICL_STS,       CHG_STS,   CHG_EN,   CHG_CFG2, CHG_WD_SNARL,
        PWR_PATH_STS, USB_IN_STATUS, APSD_STS,  APSD_RES, USB_STS,  USB_SUSP,
        CHG_FLOAT,    CHG_PRE,       CHG_FCC,   CHG_FAST,
    };

    UCHAR  v[RTL_NUMBER_OF(regs)] = { 0 }, cap[2] = { 0 };
    USHORT vraw = 0, iraw = 0;
    ULONG  fcc = 0, dcap = 0, rc = 0, i;

    for (i = 0; i < RTL_NUMBER_OF(regs); i++)
        spmi_rd(c, regs[i], &v[i], 1);

    // топливомер читаем сырым как есть без fg_fix и пересчетов
    rdw(c, FG_VBAT, &vraw);
    rdw(c, FG_IBAT, &iraw);
    spmi_rd(c, FG_SOC, cap, 2);

    // bq27411 сырье читаем тож напрямую
    i2c_rd(c, BQ27411_REG_FCC,  &fcc);
    i2c_rd(c, BQ27411_REG_DCAP, &dcap);
    i2c_rd(c, BQ27411_REG_RC,   &rc);

    DbgLog("diag: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X"
           " | %02X %02X %02X %02X"
           " | %04X %04X %02X %02X"
           " | %lu %lu %ld %u %lu"
           " | %u %lu %u %u"
           " | %lu %lu %lu %lu %lu %lu",
           v[0], v[1], v[2], v[3], v[4],
           v[5], v[6], v[7], v[8], v[9], v[10],
           v[11], v[12], v[13], v[14], v[15], v[16],
           v[17], v[18], v[19], v[20],
           vraw, iraw, cap[0], cap[1],
           c->Hw.Volt, c->Hw.Soc, c->Hw.Curr, c->Hw.Ac, c->Hw.Temp,
           c->Hw.Valid, c->SpmiFail, c->Fast, c->ChgOff,
           fcc, dcap, rc, c->Full, c->Design, c->Hw.Cap);
}

// -=-=-=-=-=-=-=-= опрос =-=-=-=-=-=-=-=

/*
    опрос батареи
    чтение -> коммит в Windows -> зарядка
*/
VOID HwPoll(POP6_CTX c)
{
    UCHAR chgsts;
    BOOLEAN stop = FALSE;

    read_hw(c);
    BattCommit(c);
    LedUpdateBattery(c);

    // Батарея несъемная - состояния батареи нет делать не нужно сдесь

    if (!c->Hw.Ac) {
        c->Fast = FALSE;
    } else {

        /*
            feed WATCHDOG каждый опрос при воткнутом кабеле
            прямая запись 0x01 (не RMW)
            как в стоковом wdog_bark_irq_handler
        */
        wrb(c, CHG_WD_PET, MASK_NONE, CHG_WD_PET_VAL);

        /*
            гасим любой активный заряд а не только наш Fast
            Fast теряется в SpmiTeardown а заряд мог включить загрузчик
            CHG_STS любое состояние кроме DISABLE(7) = зарядчик работает
        */
        if (!temp_in(c, FALSE)) {
            stop = c->Fast;

            if (!stop) {
                if (spmi_rd(c, CHG_STS, &chgsts, 1))
                    stop = (chgsts & CHG_STS_MASK) != DISABLE_CHARGE;
            }
        }

        if (stop) {
            /*
                шла зарядка и температура ушла из окна или не прочитана
                значит гасим CHG_EN и снимаем Fast

                Fast снимаем только при успешной записи иначе следующий опрос попробует снова

                ChgOff значит CHG_EN погасили мы сами и HwExit потом включит его обратно
            */
            if (wrb(c, CHG_EN, MASK_EN, 0)) {
                c->Fast   = FALSE;
                c->ChgOff = TRUE;
            }
        } else {
            unlock(c);
        }
    }

    // дамп состояния зарядника
    hw_diag(c);
}
