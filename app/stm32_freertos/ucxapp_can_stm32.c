/* CAN backend for the STM32 boards: bxCAN CAN1 on the F4/F7, FDCAN1 (classic CAN) on the H7, programmed on the
 * registers so the shared HAL configuration stays as it is. An external 3.3 V transceiver (e.g. SN65HVD230) goes
 * between the pins and the bus; loopback mode works without one.
 *   NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, STM32F4DISCOVERY: RX PD0, TX PD1
 *   NUCLEO-F446RE: RX PB8 (D15), TX PB9 (D14)  (PA11/PA12 are the module's CTS/RTS) */

#include <stdio.h>
#include <string.h>

#include "ucxapp_can.h"
#include "ucxapp_platform.h"

#include "FreeRTOS.h"
#include "task.h"

#if defined(STM32H753xx)
#include "stm32h7xx_hal.h"
#elif defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#else
#include "stm32f4xx_hal.h"
#endif

#define CAN_IRQ_PRIORITY 6
#define CAN_INIT_TIMEOUT_MS 100

#if defined(STM32F446xx)
#define CAN_GPIO      GPIOB
#define CAN_GPIO_ON() __HAL_RCC_GPIOB_CLK_ENABLE()
#define CAN_PINS      (GPIO_PIN_8 | GPIO_PIN_9)
#define CAN_PIN_NAMES "RX PB8 (D15), TX PB9 (D14)"
#else
#define CAN_GPIO      GPIOD
#define CAN_GPIO_ON() __HAL_RCC_GPIOD_CLK_ENABLE()
#define CAN_PINS      (GPIO_PIN_0 | GPIO_PIN_1)
#define CAN_PIN_NAMES "RX PD0, TX PD1"
#endif

typedef struct {
    uint32_t presc;
    uint32_t ts1;
    uint32_t ts2;
} timing_t;

static char gErr[64];

/* Prescaler and segments for a sample point near 87.5 %, with as many time quanta (8..25) as the clock allows. */
static bool bitTiming(uint32_t clk, uint32_t rate, uint32_t maxPresc, uint32_t maxTs1, uint32_t maxTs2, timing_t *t)
{
    for (uint32_t n = 25; n >= 8; n--) {
        if (clk % (rate * n) != 0) {
            continue;
        }
        uint32_t presc = clk / (rate * n);
        uint32_t ts2 = n - (n * 7 + 4) / 8;
        uint32_t ts1 = n - 1 - ts2;
        if (presc >= 1 && presc <= maxPresc && ts1 >= 1 && ts1 <= maxTs1 && ts2 >= 1 && ts2 <= maxTs2) {
            t->presc = presc;
            t->ts1 = ts1;
            t->ts2 = ts2;
            return true;
        }
    }
    return false;
}

static const char *noTiming(uint32_t clk)
{
    snprintf(gErr, sizeof(gErr), "the bit rate cannot be made from the %u Hz CAN clock", (unsigned)clk);
    return gErr;
}

static bool waitFor(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    uint32_t t0 = ucxapp_platform_tick_ms();

    while ((*reg & mask) != value) {
        if (ucxapp_platform_tick_ms() - t0 > CAN_INIT_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

static void pins(uint8_t af)
{
    GPIO_InitTypeDef g = { 0 };

    CAN_GPIO_ON();
    g.Pin = CAN_PINS;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;   /* RX reads recessive when no transceiver is fitted */
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = af;
    HAL_GPIO_Init(CAN_GPIO, &g);
}

static void unpack(uint32_t lo, uint32_t hi, ucxapp_can_frame_t *f)
{
    for (int i = 0; i < 4; i++) {
        f->data[i] = (uint8_t)(lo >> (8 * i));
        f->data[i + 4] = (uint8_t)(hi >> (8 * i));
    }
}

static uint32_t word(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

#if defined(STM32H753xx)

/* ---- FDCAN1 in classic CAN mode, kernel clock HSE (8 MHz from the ST-LINK) ---- */

#define RAM_RX_OFF  0u     /* RX FIFO 0: 16 elements of 4 words (8-byte data) */
#define RAM_RX_N    16u
#define RAM_TX_OFF  256u   /* TX FIFO: 8 elements */
#define RAM_TX_N    8u
#define RAM_ELEM    16u

static volatile uint32_t *ramElem(uint32_t off, uint32_t idx)
{
    return (volatile uint32_t *)(SRAMCAN_BASE + off + idx * RAM_ELEM);
}

const char *ucxapp_can_hw_name(void)
{
    return "FDCAN1 (classic), " CAN_PIN_NAMES;
}

const char *ucxapp_can_hw_start(int32_t bitrate, ucxapp_can_mode_t mode)
{
    timing_t t;
    volatile uint32_t *ram = (volatile uint32_t *)SRAMCAN_BASE;

    if (!bitTiming(HSE_VALUE, (uint32_t)bitrate, 512, 256, 128, &t)) {
        return noTiming(HSE_VALUE);
    }
    NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
    __HAL_RCC_FDCAN_CLK_ENABLE();
    __HAL_RCC_FDCAN_FORCE_RESET();
    __HAL_RCC_FDCAN_RELEASE_RESET();
    MODIFY_REG(RCC->D2CCIP1R, RCC_D2CCIP1R_FDCANSEL, 0u);   /* HSE */
    pins(GPIO_AF9_FDCAN1);

    FDCAN1->CCCR &= ~FDCAN_CCCR_CSR;
    if (!waitFor(&FDCAN1->CCCR, FDCAN_CCCR_CSA, 0u)) {
        return "FDCAN1 did not leave clock stop";
    }
    FDCAN1->CCCR |= FDCAN_CCCR_INIT;
    if (!waitFor(&FDCAN1->CCCR, FDCAN_CCCR_INIT, FDCAN_CCCR_INIT)) {
        return "FDCAN1 did not enter initialisation";
    }
    FDCAN1->CCCR |= FDCAN_CCCR_CCE;
    FDCAN1->CCCR &= ~(FDCAN_CCCR_FDOE | FDCAN_CCCR_BRSE | FDCAN_CCCR_DAR | FDCAN_CCCR_TEST | FDCAN_CCCR_MON |
                      FDCAN_CCCR_ASM);
    if (mode == UCXAPP_CAN_LOOPBACK) {
        FDCAN1->CCCR |= FDCAN_CCCR_TEST | FDCAN_CCCR_MON;   /* internal loopback: the TX pin stays recessive */
        FDCAN1->TEST |= FDCAN_TEST_LBCK;
    } else if (mode == UCXAPP_CAN_LISTEN) {
        FDCAN1->CCCR |= FDCAN_CCCR_MON;   /* bus monitoring: receives, never drives the bus */
    }
    FDCAN1->NBTP = (0u << FDCAN_NBTP_NSJW_Pos) | ((t.presc - 1u) << FDCAN_NBTP_NBRP_Pos) |
                   ((t.ts1 - 1u) << FDCAN_NBTP_NTSEG1_Pos) | ((t.ts2 - 1u) << FDCAN_NBTP_NTSEG2_Pos);

    for (uint32_t i = 0; i < (RAM_TX_OFF + RAM_TX_N * RAM_ELEM) / 4u; i++) {
        ram[i] = 0;
    }
    FDCAN1->SIDFC = 0;
    FDCAN1->XIDFC = 0;
    FDCAN1->GFC = 0;   /* no filters: every standard and extended frame goes to RX FIFO 0 */
    FDCAN1->RXF0C = ((RAM_RX_OFF / 4u) << FDCAN_RXF0C_F0SA_Pos) | (RAM_RX_N << FDCAN_RXF0C_F0S_Pos);
    FDCAN1->RXF1C = 0;
    FDCAN1->RXBC = 0;
    FDCAN1->RXESC = 0;
    FDCAN1->TXEFC = 0;
    FDCAN1->TXESC = 0;
    FDCAN1->TXBC = ((RAM_TX_OFF / 4u) << FDCAN_TXBC_TBSA_Pos) | (RAM_TX_N << FDCAN_TXBC_TFQS_Pos);
    FDCAN1->IE = FDCAN_IE_RF0NE;
    FDCAN1->ILS = 0;
    FDCAN1->ILE = FDCAN_ILE_EINT0;
    FDCAN1->IR = 0xFFFFFFFFu;

    FDCAN1->CCCR &= ~FDCAN_CCCR_INIT;
    if (!waitFor(&FDCAN1->CCCR, FDCAN_CCCR_INIT, 0u)) {
        return "FDCAN1 did not join the bus (RX pin held dominant? check the transceiver)";
    }
    NVIC_SetPriority(FDCAN1_IT0_IRQn, CAN_IRQ_PRIORITY);
    NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    return NULL;
}

bool ucxapp_can_hw_send(const ucxapp_can_frame_t *f)
{
    uint32_t idx;
    volatile uint32_t *e;
    uint8_t len = f->len > 8 ? 8 : f->len;

    if ((FDCAN1->TXFQS & FDCAN_TXFQS_TFQF) != 0u) {
        return false;
    }
    idx = (FDCAN1->TXFQS & FDCAN_TXFQS_TFQPI_Msk) >> FDCAN_TXFQS_TFQPI_Pos;
    e = ramElem(RAM_TX_OFF, idx);
    e[0] = f->id > 0x7FFu ? ((f->id & 0x1FFFFFFFu) | (1u << 30)) : (f->id << 18);
    e[1] = (uint32_t)len << 16;
    e[2] = word(f->data);
    e[3] = word(f->data + 4);
    FDCAN1->TXBAR = 1u << idx;
    return true;
}

void ucxapp_can_hw_state(ucxapp_can_hw_state_t *st)
{
    uint32_t psr = FDCAN1->PSR;
    uint32_t ecr = FDCAN1->ECR;

    st->tec = (uint8_t)((ecr >> FDCAN_ECR_TEC_Pos) & 0xFFu);
    st->rec = (uint8_t)((ecr >> FDCAN_ECR_REC_Pos) & 0x7Fu);
    st->state = (psr & FDCAN_PSR_BO) != 0u ? "bus-off" : (psr & FDCAN_PSR_EP) != 0u ? "error-passive"
              : (psr & FDCAN_PSR_EW) != 0u ? "error-warning" : "error-active";
}

void FDCAN1_IT0_IRQHandler(void);
void FDCAN1_IT0_IRQHandler(void)
{
    FDCAN1->IR = FDCAN_IR_RF0N;
    while ((FDCAN1->RXF0S & FDCAN_RXF0S_F0FL) != 0u) {
        uint32_t idx = (FDCAN1->RXF0S & FDCAN_RXF0S_F0GI_Msk) >> FDCAN_RXF0S_F0GI_Pos;
        volatile uint32_t *e = ramElem(RAM_RX_OFF, idx);
        uint32_t w0 = e[0];
        uint32_t dlc = (e[1] >> 16) & 0xFu;
        ucxapp_can_frame_t f;

        f.id = (w0 & (1u << 30)) != 0u ? (w0 & 0x1FFFFFFFu) : ((w0 >> 18) & 0x7FFu);
        f.len = (uint8_t)(dlc > 8u ? 8u : dlc);
        unpack(e[2], e[3], &f);
        FDCAN1->RXF0A = idx;
        ucxapp_can_rx_isr(&f);
    }
}

#else

/* ---- bxCAN CAN1 on APB1 ---- */

const char *ucxapp_can_hw_name(void)
{
    return "bxCAN CAN1, " CAN_PIN_NAMES;
}

const char *ucxapp_can_hw_start(int32_t bitrate, ucxapp_can_mode_t mode)
{
    timing_t t;
    uint32_t clk = HAL_RCC_GetPCLK1Freq();

    if (!bitTiming(clk, (uint32_t)bitrate, 1024, 16, 8, &t)) {
        return noTiming(clk);
    }
    NVIC_DisableIRQ(CAN1_RX0_IRQn);
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_CAN1_FORCE_RESET();
    __HAL_RCC_CAN1_RELEASE_RESET();
    pins(GPIO_AF9_CAN1);

    CAN1->MCR = CAN_MCR_INRQ | CAN_MCR_ABOM | CAN_MCR_TXFP;   /* leaves sleep; bus-off recovers by itself */
    if (!waitFor(&CAN1->MSR, CAN_MSR_INAK | CAN_MSR_SLAK, CAN_MSR_INAK)) {
        return "CAN1 did not enter initialisation";
    }
    CAN1->BTR = ((t.ts2 - 1u) << 20) | ((t.ts1 - 1u) << 16) | (t.presc - 1u) |
                (mode == UCXAPP_CAN_LOOPBACK ? (CAN_BTR_LBKM | CAN_BTR_SILM)   /* silent loopback: nothing on the bus */
                 : mode == UCXAPP_CAN_LISTEN ? CAN_BTR_SILM : 0u);

    CAN1->FMR |= CAN_FMR_FINIT;
    CAN1->FA1R &= ~1u;
    CAN1->FS1R |= 1u;      /* bank 0: one 32-bit mask filter */
    CAN1->FM1R &= ~1u;
    CAN1->FFA1R &= ~1u;    /* to FIFO 0 */
    CAN1->sFilterRegister[0].FR1 = 0;
    CAN1->sFilterRegister[0].FR2 = 0;   /* mask 0: accept every frame */
    CAN1->FA1R |= 1u;
    CAN1->FMR &= ~CAN_FMR_FINIT;

    CAN1->IER = CAN_IER_FMPIE0;
    CAN1->MCR &= ~CAN_MCR_INRQ;
    if (!waitFor(&CAN1->MSR, CAN_MSR_INAK, 0u)) {
        return "CAN1 did not join the bus (RX pin held dominant? check the transceiver)";
    }
    NVIC_SetPriority(CAN1_RX0_IRQn, CAN_IRQ_PRIORITY);
    NVIC_EnableIRQ(CAN1_RX0_IRQn);
    return NULL;
}

bool ucxapp_can_hw_send(const ucxapp_can_frame_t *f)
{
    uint32_t tsr = CAN1->TSR;
    int mb = (tsr & CAN_TSR_TME0) != 0u ? 0 : (tsr & CAN_TSR_TME1) != 0u ? 1 : (tsr & CAN_TSR_TME2) != 0u ? 2 : -1;
    uint8_t len = f->len > 8 ? 8 : f->len;
    CAN_TxMailBox_TypeDef *m;

    if (mb < 0) {
        return false;
    }
    m = &CAN1->sTxMailBox[mb];
    m->TIR = f->id > 0x7FFu ? (((f->id & 0x1FFFFFFFu) << 3) | CAN_TI0R_IDE) : (f->id << 21);
    m->TDTR = len;
    m->TDLR = word(f->data);
    m->TDHR = word(f->data + 4);
    m->TIR |= CAN_TI0R_TXRQ;
    return true;
}

void ucxapp_can_hw_state(ucxapp_can_hw_state_t *st)
{
    uint32_t esr = CAN1->ESR;

    st->tec = (uint8_t)(esr >> 16);
    st->rec = (uint8_t)(esr >> 24);
    st->state = (esr & CAN_ESR_BOFF) != 0u ? "bus-off" : (esr & CAN_ESR_EPVF) != 0u ? "error-passive"
              : (esr & CAN_ESR_EWGF) != 0u ? "error-warning" : "error-active";
}

void CAN1_RX0_IRQHandler(void);
void CAN1_RX0_IRQHandler(void)
{
    while ((CAN1->RF0R & CAN_RF0R_FMP0) != 0u) {
        CAN_FIFOMailBox_TypeDef *m = &CAN1->sFIFOMailBox[0];
        uint32_t rir = m->RIR;
        uint32_t dlc = m->RDTR & 0xFu;
        ucxapp_can_frame_t f;

        f.id = (rir & CAN_RI0R_IDE) != 0u ? (rir >> 3) : (rir >> 21);
        f.len = (uint8_t)(dlc > 8u ? 8u : dlc);
        unpack(m->RDLR, m->RDHR, &f);
        CAN1->RF0R = CAN_RF0R_RFOM0;
        ucxapp_can_rx_isr(&f);
    }
}

#endif

/* ---- can.bridge task: below the console task, so commands stay responsive while it runs ---- */

#define CAN_TASK_WORDS    512
#define CAN_TASK_PRIORITY 3

static void (*gTaskFn)(void);

static void taskEntry(void *arg)
{
    (void)arg;
    gTaskFn();
    vTaskDelete(NULL);
}

bool ucxapp_can_hw_task(void (*fn)(void))
{
    gTaskFn = fn;
    return xTaskCreate(taskEntry, "can", CAN_TASK_WORDS, NULL, CAN_TASK_PRIORITY, NULL) == pdPASS;
}
