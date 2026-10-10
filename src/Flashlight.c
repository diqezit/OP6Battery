#include <ntddk.h>
#include <wdf.h>

#include "Op6Battery.h"

// -=-=-=-=-=-=-=-= запись FLASH =-=-=-=-=-=-=-=

/*
    свой wr а не wrb

    wrb требует HwOk - это опознание топливомера по PERPH_SUBTYPE
    фонарик сидит в другом блоке PMIC (FLASH 0xD300) и к FG не относится
    шина открыта а FG не ответил - зарядник молчит а фонарик должен работать

    успех только st == SPMI_SUCCESS тот же контракт что в spmi_rd
*/
static BOOLEAN torch_wr(POP6_CTX c, USHORT reg, UCHAR val)
{
    NTSTATUS st;

    if (!c->SpmiWrite) return FALSE;

    st = c->SpmiWrite(c->SpmiBus, 0, SPMI_ADDR(reg), &val, 1);
    if (st != SPMI_SUCCESS) {
        DbgLog("torch_wr failed reg=0x%04X st=0x%08X", reg, st);
        return FALSE;
    }
    return TRUE;
}

// 1..100 -> сырой ток 1..TORCH_MAX_RAW (200 мА / 12.5)
static UCHAR torch_raw(UCHAR pct)
{
    ULONG raw;

    if (!pct) return 0;

    raw = ((ULONG)pct * TORCH_MAX_RAW + 50) / 100;
    if (!raw) raw = 1;
    if (raw > TORCH_MAX_RAW) raw = TORCH_MAX_RAW;
    return (UCHAR)raw;
}

// -=-=-=-=-=-=-=-= выключение =-=-=-=-=-=-=-=

/*
    сначала строб потом ток
    иначе диод может мигнуть на остаточном токе без строба наоборот не бывает
    шины нет - не открываем: зовут из HwExit перед Teardown
*/
VOID TorchOff(POP6_CTX c)
{
    if (!c->SpmiWrite) {
        c->TorchOn = FALSE;
        return;
    }

    (VOID)torch_wr(c, FLASH_STROBE_CTRL, 0);
    (VOID)torch_wr(c, FLASH_LED1_CUR, 0);
    (VOID)torch_wr(c, FLASH_LED2_CUR, 0);
    c->TorchOn = FALSE;
    DbgLog("torch: off");
}

// -=-=-=-=-=-=-=-= включение =-=-=-=-=-=-=-=

/*
    не вспышка камеры
    flash это 1000 мА и короткий safety timer
    torch это 200 мА на диод без таймера пока юзермод не попросит выкл

    порядок как в qpnp-flash-led-v2
      1) ток и лимиты
      2) IRES 12.5 мА
      3) программный строб
      4) STROBE_CTRL последним - без него диоды не зажигаются

    любой сбой в середине - гасим строб чтобы не оставить дыру
*/
NTSTATUS TorchSet(POP6_CTX c, UCHAR brightness)
{
    UCHAR raw;

    if (!brightness) {
        TorchOff(c);
        return STATUS_SUCCESS;
    }

    // IOCTL может прийти до первого тика таймера
    ensure_spmi(c);
    if (!c->SpmiWrite)
        return STATUS_DEVICE_NOT_READY;

    raw = torch_raw(brightness);

    // таймер выкл: иначе через секунды сами погаснем как flash
    if (!torch_wr(c, FLASH_SAFETY_TIMER, 0) ||
        !torch_wr(c, FLASH_MAX_CURRENT, TORCH_MAX_RAW) ||
        !torch_wr(c, FLASH_CLAMP_CUR, TORCH_MAX_RAW) ||
        !torch_wr(c, FLASH_IRES, TORCH_IRES_VAL) ||
        !torch_wr(c, FLASH_LED1_CUR, raw) ||
        !torch_wr(c, FLASH_LED2_CUR, raw) ||
        !torch_wr(c, FLASH_STROBE_CFG, 0) ||
        !torch_wr(c, FLASH_STROBE_CTRL, TORCH_STROBE_LEDS)) {

        (VOID)torch_wr(c, FLASH_STROBE_CTRL, 0);
        c->TorchOn = FALSE;
        return STATUS_IO_DEVICE_ERROR;
    }

    c->TorchOn = TRUE;
    DbgLog("torch: on raw=%u pct=%u", raw, brightness);
    return STATUS_SUCCESS;
}

// -=-=-=-=-=-=-=-= управление светодиодом (LPG + Tri-LED Slave 3) =-=-=-=-=-=-=-=

/*
    RGB-индикатор: LPG канал на каждый цвет делает ШИМ
    Tri-LED подключает выбранные каналы к источнику питания
    адреса регистров маски и значения вынесены в Op6Battery.h
*/

enum {
    LED_ST_OFF = 0,     // 0 = OFF
    LED_ST_RED,         // 1 = RED     Soc < LED_SOC_RED
    LED_ST_YELLOW,      // 2 = YELLOW  Soc < LED_SOC_GREEN
    LED_ST_GREEN,       // 3 = GREEN
};

#define LED_RGB_MAX        255
#define LED_RGB_OFF          0
#define LED_YELLOW_G       140
#define LPG_DUTY_MSB_SHIFT   8
#define TRI_LED_SRC_OFF      0

// слейв 3 в старшем байте как SPMI_ADDR но LED_SLAVE а не SLAVE
#define LED_ADDR(reg) (((ULONG)LED_SLAVE << 16) | (USHORT)(reg))

// порядок как в qpnp-lpg тактирование до скважности
//
// SIZE_CLK первой строкой: 32 кГц и 9 бит -> ШИМ 64 Гц ровный свет
// без этого канал мигает на дефолтной частоте LPG
// PREDIV div 1 exp 1 и TYPE как в mainline qpnp-lpg
// скважность enable SYNC не здесь: duty зависит от яркости
// SYNC пишется последним иначе новые значения не защёлкиваются
static const SPMI_WRITE lpg_seq[] = {
    { LPG_PWM_SIZE_CLK,   MASK_NONE, LPG_VAL_SIZE_CLK },  // 32 кГц, 9 бит -> ШИМ 64 Гц
    { LPG_PWM_PREDIV_CLK, MASK_NONE, LPG_VAL_PREDIV   },  // div 1 exp 1
    { LPG_PWM_TYPE_CTL,   MASK_NONE, LPG_VAL_TYPE     },  // конфигурация типа
};

// RGB: база канала LPG и бит Tri-LED
// порядок R G B совпадает с аргументами LedSetRgb
static const SPMI_WRITE led_chan[] = {
    { LPG_RED_BASE,   MASK_NONE, TRI_LED_EN_RED   },  // LPG 5 красный
    { LPG_GREEN_BASE, MASK_NONE, TRI_LED_EN_GREEN },  // LPG 4 зеленый
    { LPG_BLUE_BASE,  MASK_NONE, TRI_LED_EN_BLUE  },  // LPG 3 синий
};

// автоцвет: индекс = LED_ST_*
static const UCHAR led_rgb[][3] = {
    { LED_RGB_OFF, LED_RGB_OFF,  LED_RGB_OFF },  // выкл
    { LED_RGB_MAX, LED_RGB_OFF,  LED_RGB_OFF },  // красный  Soc < LED_SOC_RED
    { LED_RGB_MAX, LED_YELLOW_G, LED_RGB_OFF },  // желтый   Soc < LED_SOC_GREEN
    { LED_RGB_OFF, LED_RGB_MAX,  LED_RGB_OFF },  // зеленый
};

/*
    свой wr а не wrb и не torch_wr

    wrb требует HwOk - опознание FG по PERPH_SUBTYPE
    torch_wr ходит на SLAVE 2 (FLASH 0xD300)
    индикатор сидит на SLAVE 3 (LPG + Tri-LED) и к FG не относится
    шина открыта а FG не ответил - зарядник молчит а светодиод должен работать

    успех только st == SPMI_SUCCESS тот же контракт что в spmi_rd
*/
static BOOLEAN led_wr(POP6_CTX c, USHORT reg, UCHAR val)
{
    NTSTATUS st;

    if (!c->SpmiWrite) return FALSE;

    st = c->SpmiWrite(c->SpmiBus, 0, LED_ADDR(reg), &val, 1);
    if (st != SPMI_SUCCESS) {
        DbgLog("led_wr failed reg=0x%04X st=0x%08X", reg, st);
        return FALSE;
    }
    return TRUE;
}

/*
    скважность 9 бит лежит в двух регистрах
    LSB биты 0..7 MSB бит 8
    пишем парой чтобы канал не мигнул на половинном значении
*/
static VOID lpg_duty(POP6_CTX c, USHORT base, USHORT duty)
{
    (VOID)led_wr(c, (USHORT)(base + LPG_PWM_VALUE_LSB),
                 (UCHAR)(duty & LPG_DUTY_LSB_MASK));
    (VOID)led_wr(c, (USHORT)(base + LPG_PWM_VALUE_MSB),
                 (UCHAR)((duty >> LPG_DUTY_MSB_SHIFT) & LPG_DUTY_MSB_MASK));
}

/*
    один канал LPG: тактирование скважность включение

    порядок как в qpnp-lpg
      1) SIZE_CLK PREDIV TYPE из таблицы lpg_seq
      2) скважность
      3) ENABLE
      4) на PMI8998 скважность ещё раз после ENABLE (кремниевый баг)
      5) SYNC последним - без него новые значения не защёлкиваются

    duty512 = 0 канал гасим но бит 5 в ENABLE оставляем как в вендоре
*/
static VOID lpg_chan_config(POP6_CTX c, USHORT base, USHORT duty512)
{
    ULONG i;

    if (duty512 > LPG_DUTY_MAX)
        duty512 = LPG_DUTY_MAX;

    for (i = 0; i < RTL_NUMBER_OF(lpg_seq); i++)
        (VOID)led_wr(c, (USHORT)(base + lpg_seq[i].reg), lpg_seq[i].val);

    // Начальные значения ШИМ
    lpg_duty(c, base, duty512);

    // Включение канала
    if (duty512) {
        (VOID)led_wr(c, (USHORT)(base + LPG_PWM_ENABLE_CTL), LPG_VAL_EN_ON);

        // Обход кремниевого бага
        // перезапись скважности после включения
        lpg_duty(c, base, duty512);
    } else {
        (VOID)led_wr(c, (USHORT)(base + LPG_PWM_ENABLE_CTL), LPG_VAL_EN_OFF);
    }

    // Синхронизация ШИМ
    (VOID)led_wr(c, (USHORT)(base + LPG_PWM_SYNC), LPG_VAL_SYNC);
}

/*
    RGB 0..255 на три канала потом Tri-LED
    (0,0,0) = выкл: гасим каналы и отключаем питание VPH_PWR
    шины нет - тихий выход: зовут из HwExit перед Teardown
*/
VOID LedSetRgb(POP6_CTX c, UCHAR r, UCHAR g, UCHAR b)
{
    UCHAR rgb[3] = { r, g, b };
    UCHAR triEn = 0;
    UCHAR src = TRI_LED_SRC_OFF;
    ULONG i;

    if (!c || !c->SpmiWrite || !c->SpmiBus)
        return;

    for (i = 0; i < RTL_NUMBER_OF(led_chan); i++) {
        lpg_chan_config(c, led_chan[i].reg,
            (USHORT)(((ULONG)rgb[i] * LPG_DUTY_MAX) / LED_RGB_MAX));
        if (rgb[i])
            triEn |= led_chan[i].val;
    }

    // Источник питания: VPH_PWR если каналы включены иначе выкл
    if (triEn)
        src = TRI_LED_SRC_VPH_PWR;
    (VOID)led_wr(c, TRI_LED_SRC, src);

    // Включение линий в Tri-LED
    (VOID)led_wr(c, TRI_LED_EN, triEn);
}

/*
    автоцвет только при живой телеметрии и без ручного IOCTL
    кабель есть:  RED <20  YELLOW <90  GREEN >=90
    кабеля нет:   выкл
    пишем железо только при смене состояния иначе каждый опрос мигает
    зовет HwPoll
*/
VOID LedUpdateBattery(POP6_CTX c)
{
    UCHAR state = LED_ST_OFF; // 0 = OFF, 1 = RED, 2 = YELLOW, 3 = GREEN

    if (!c || !c->SpmiWrite || !c->SpmiBus || !c->Hw.Valid)
        return;

    if (c->LedManualOverride)
        return;

    if (c->Hw.Ac) {
        if (c->Hw.Soc < LED_SOC_RED)
            state = LED_ST_RED;         // Красный (< 20%)
        else if (c->Hw.Soc < LED_SOC_GREEN)
            state = LED_ST_YELLOW;      // Желтый (20% - 89%)
        else
            state = LED_ST_GREEN;       // Зеленый (>= 90%)
    }

    if (state == c->LastLedState)
        return;

    c->LastLedState = state;
    LedSetRgb(c, led_rgb[state][0], led_rgb[state][1], led_rgb[state][2]);
    DbgLog("Led: battery state=%u soc=%lu ac=%u", state, c->Hw.Soc, c->Hw.Ac);
}
