#include "main.h"

#define MOTOR_TIMER_HZ 1000U
#define AUTO_INTERVAL_MS 5000U
#define AUTO_START_DIRECTION 1 /* 1 = 上电先正转, -1 = 先反转 */

/* HC-160A S2 dual H-bridge.
 * Channel 1: direction inputs "A"/"B", speed input "P" (PA8).
 * Channel 2: direction inputs "a"/"b", speed input "P" (PA11).
 * Vendor table: A=1,B=0 forward; A=0,B=1 reverse; A=0,B=0 brake; A=1,B=1 UNDEFINED.
 *
 * The speed inputs take a PWM SQUARE WAVE, not a DC level. Vendor text, verbatim:
 *   "PA为PWM波输入；频率最高为60KHZ；占空比最高达98%"
 * A static HIGH is 100% duty, which is outside that 98% limit, and the module then
 * refuses to drive the bridge at all - measured with a static high on both speed
 * pins: both motor terminals sat at 0V no matter what the direction pins did, and
 * jumpering the speed pin straight to the module's own 5V output changed nothing.
 * So both speed inputs are driven by TIM1 as real square waves at the vendor's
 * maximum permitted duty. There is still no speed control in this program: the
 * duty cycle is a compile-time constant and nothing ever changes it at runtime.
 *
 * These six pins are all 5V-tolerant (FT) parts of the F103. PA0-PA7, PB0, PB1 and
 * PC13-PC15 are NOT FT, and every GPIO is a floating input for the ~2-3 ms between
 * reset release and GPIO_Init(). Measured on this board there is NO 5V pull-up on
 * the control inputs (they rest at ~0.2V with the module powered), so a non-FT pin
 * would in fact have been fine here. The FT choice is a conservative one, not a fix
 * for an observed fault - but the cost of keeping it is zero, so keep it. */
#define M1_DIR_A_PIN GPIO_PIN_8 /* ch1 direction "A" (was PB0) */
#define M1_DIR_B_PIN GPIO_PIN_9 /* ch1 direction "B" (was PB1) */
#define M2_DIR_A_PIN GPIO_PIN_6 /* ch2 direction "a" */
#define M2_DIR_B_PIN GPIO_PIN_7 /* ch2 direction "b" */
#define DIR_PORT GPIOB          /* all four direction pins must share this port: dir_write() relies on one BSRR store */
#define M1_PWM_PIN GPIO_PIN_8   /* ch1 speed "P", PA8 = TIM1_CH1 */
#define M2_PWM_PIN GPIO_PIN_11  /* ch2 speed "P", PA11 = TIM1_CH4 */
#define PWM_PORT GPIOA

/* Speed-input PWM. 15 kHz is what the vendor's own wiring diagram annotates the "P"
 * pins with ("15K方波"); the module accepts up to 60 kHz. Duty is pinned to the 98%
 * maximum the vendor specifies: at 98% the motor sees 0.98 x Vs, which is full speed
 * for any practical purpose, while staying inside spec. Do NOT raise this to 100% -
 * that is precisely the failure this rewrite exists to fix. Halving it is a valid
 * way to make the motors run at roughly half speed, but it is not a calibrated
 * speed control. */
#define PWM_FREQ_HZ 15000U
#define PWM_DUTY_PERCENT 98U
#define PWM_TIMER_CLOCK_HZ 64000000U /* APB2 prescaler is 1, so TIM1CLK = PCLK2 = SYSCLK */
#define PWM_ARR (PWM_TIMER_CLOCK_HZ / PWM_FREQ_HZ - 1U)          /* 4265 -> 15.0 kHz */
#define PWM_CCR ((PWM_ARR + 1U) * PWM_DUTY_PERCENT / 100U)       /* 4180 -> 98.0 % */
#define LIMIT_DOWN_PIN GPIO_PIN_10
#define LIMIT_UP_PIN GPIO_PIN_11
#define BUTTON_UP_PIN GPIO_PIN_12
#define BUTTON_DOWN_PIN GPIO_PIN_13

/* Debug switch: set to 0 to bench-test with channel 2 parked (0% duty on TIM1_CH4, a/b braked). */
#define CHANNEL_B_ENABLED 1

/* Milliseconds to brake (A=B=0) before each reversal. 0 = reverse immediately, no dead
 * time. Reversing instantly puts supply and back-EMF in series, so the current peak is
 * about 2x stall; braking first cuts it to about 1x stall and lets it decay. It does not
 * remove the surge - the brake phase itself draws back-EMF/winding-resistance.
 * Cycle becomes 5000 + REVERSE_DEAD_TIME_MS once this is non-zero.
 * Raise to 200U if the supply sags on reversal (UART repeating "RESET: POR/BROWNOUT"). */
#define REVERSE_DEAD_TIME_MS 0U

TIM_HandleTypeDef htim1; /* PWM only: the two HC-160A S2 speed inputs */
TIM_HandleTypeDef htim3; /* 1 kHz millisecond tick only */
UART_HandleTypeDef huart1;
static volatile uint8_t motor_ready = 0U;
static volatile int8_t a_direction = 0;
static volatile int8_t b_direction = 0;
static volatile uint32_t a_remaining_ms = 0U;
static volatile uint32_t b_remaining_ms = 0U;
static volatile uint8_t auto_mode = 1U;
static volatile int8_t auto_direction = AUTO_START_DIRECTION;
static volatile uint32_t auto_remaining_ms = AUTO_INTERVAL_MS;
static volatile uint8_t auto_braking = 0U; /* 1 = inside the pre-reversal dead time */
static volatile uint32_t dead_time_remaining = 0U;
static volatile int8_t pending_direction = AUTO_START_DIRECTION;

static void SystemClock_Config(void);
static void GPIO_Init(void);
static void TIM1_PWM_Init(void);
static void TIM3_Init(void);
static void USART1_Init(void);
static void motor_stop_a(void);
static void motor_stop_b(void);
static void motor_set_a(int8_t direction);
static void motor_set_b(int8_t direction);
static void process_command(const char *command);
static void uart_puts(const char *s);

static uint8_t limit_active(uint16_t pin)
{
  return HAL_GPIO_ReadPin(GPIOB, pin) == GPIO_PIN_RESET;
}

static uint32_t parse_number(const char *p)
{
  uint32_t value = 0U;
  while (*p >= '0' && *p <= '9') {
    value = value * 10U + (uint32_t)(*p - '0');
    if (value > 3600000U) return 3600000U;
    ++p;
  }
  return value;
}

/* Drive one channel's two direction inputs with a single BSRR store.
 * Writing them with two HAL_GPIO_WritePin() calls leaves a window where both read
 * high ("A=1,B=1" - a state the vendor table does not define) when going from
 * reverse to forward. BSRR bits 0..15 set, bits 16..31 reset; one 32-bit store is
 * atomic on Cortex-M3, so the intermediate state never appears. Both pins must be
 * on the same port. */
static void dir_write(uint16_t pin_a, uint16_t pin_b, int8_t direction)
{
  uint32_t set = 0U;
  uint32_t reset = 0U;
  if (direction > 0) { /* forward: A high, B low */
    set = pin_a;
    reset = pin_b;
  } else if (direction < 0) { /* reverse: A low, B high */
    set = pin_b;
    reset = pin_a;
  } else { /* stop: both low = brake on the HC-160A S2 */
    reset = (uint32_t)pin_a | pin_b;
  }
  DIR_PORT->BSRR = set | (reset << 16U);
}

/* "Stop" writes A=B=0, which on the HC-160A S2 is a BRAKE (windings shorted through
 * the low-side FETs), not a de-energise: the speed input keeps switching at 98%, so
 * the bridge remains live and will get warm if the load back-drives it. */
static void motor_stop_a(void)
{
  a_direction = 0;
  a_remaining_ms = 0U;
  dir_write(M1_DIR_A_PIN, M1_DIR_B_PIN, 0);
}

static void motor_stop_b(void)
{
  b_direction = 0;
  b_remaining_ms = 0U;
  dir_write(M2_DIR_A_PIN, M2_DIR_B_PIN, 0);
}

static void motor_set_a(int8_t direction)
{
  if (direction > 0 && limit_active(LIMIT_UP_PIN)) { motor_stop_a(); return; }
  if (direction < 0 && limit_active(LIMIT_DOWN_PIN)) { motor_stop_a(); return; }
  dir_write(M1_DIR_A_PIN, M1_DIR_B_PIN, direction);
}

static void motor_set_b(int8_t direction)
{
#if !CHANNEL_B_ENABLED
  (void)direction; /* keep the cast: -Wextra flags an unused parameter otherwise */
  motor_stop_b();
  return;
#else
  if (direction > 0 && limit_active(LIMIT_UP_PIN)) { motor_stop_b(); return; }
  if (direction < 0 && limit_active(LIMIT_DOWN_PIN)) { motor_stop_b(); return; }
  dir_write(M2_DIR_A_PIN, M2_DIR_B_PIN, direction);
#endif
}

static void start_motor(volatile int8_t *direction, volatile uint32_t *remaining,
                        int8_t value, uint32_t duration_ms)
{
  *direction = value;
  *remaining = duration_ms;
}

static void uart_puts(const char *s)
{
  uint16_t len = 0U;
  while (s[len] != '\0') ++len;
  if (len != 0U) (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, len, 100U);
}

/* Print why the MCU last reset, then clear the flags so the next boot is unambiguous.
 * Note the STM32F103 has no BOR (brown-out) reset flag - that is an F2/F4 feature.
 * On F103 a supply sag shows up as PORRSTF. A reset loop here means the reversal
 * surge is pulling the supply down; raise REVERSE_DEAD_TIME_MS. */
static void print_reset_cause(void)
{
  uint32_t csr = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF;
  if (csr & RCC_CSR_PORRSTF) uart_puts("RESET: POR/BROWNOUT\r\n");
  if (csr & RCC_CSR_PINRSTF) uart_puts("RESET: NRST pin\r\n");
  if (csr & RCC_CSR_SFTRSTF) uart_puts("RESET: software\r\n");
  if (csr & RCC_CSR_IWDGRSTF) uart_puts("RESET: IWDGRST\r\n");
  if (csr & RCC_CSR_WWDGRSTF) uart_puts("RESET: WWDGRST\r\n");
  if (csr & RCC_CSR_LPWRRSTF) uart_puts("RESET: low-power\r\n");
}

/* A+5000/B-3000/X+5000 are timed commands in milliseconds; A+/B+ are continuous.
   A0/B0/X0 stop (brake) channels. u/d/s are aliases for X. */
static void process_command(const char *command)
{
  char channel = command[0];
  char action = command[1];
  uint32_t duration = parse_number(&command[2]);
  int8_t direction = (action == '+') ? 1 : ((action == '-') ? -1 : 0);

  if (channel == 'r' || channel == 'R') {
    auto_mode = 1U;
    auto_direction = AUTO_START_DIRECTION;
    auto_remaining_ms = AUTO_INTERVAL_MS;
    auto_braking = 0U; /* do not resume stuck inside a dead time */
    dead_time_remaining = 0U;
    return;
  }
  if (channel == 'u' || channel == 'U') { auto_mode = 0U; start_motor(&a_direction, &a_remaining_ms, 1, 0); start_motor(&b_direction, &b_remaining_ms, 1, 0); return; }
  if (channel == 'd' || channel == 'D') { auto_mode = 0U; start_motor(&a_direction, &a_remaining_ms, -1, 0); start_motor(&b_direction, &b_remaining_ms, -1, 0); return; }
  if (channel == 's' || channel == 'S') { auto_mode = 0U; motor_stop_a(); motor_stop_b(); return; }
  if (channel != 'A' && channel != 'a' && channel != 'B' && channel != 'b' && channel != 'X' && channel != 'x') return;

  if (action == '0' || action == 's' || action == 'S') {
    auto_mode = 0U;
    if (channel == 'A' || channel == 'a' || channel == 'X' || channel == 'x') motor_stop_a();
    if (channel == 'B' || channel == 'b' || channel == 'X' || channel == 'x') motor_stop_b();
    return;
  }
  if (direction == 0) return;
  auto_mode = 0U;
  if (channel == 'A' || channel == 'a' || channel == 'X' || channel == 'x') start_motor(&a_direction, &a_remaining_ms, direction, duration);
  if (channel == 'B' || channel == 'b' || channel == 'X' || channel == 'x') start_motor(&b_direction, &b_remaining_ms, direction, duration);
}

int main(void)
{
  uint8_t rx;
  char command[24];
  uint8_t command_len = 0U;

  HAL_Init();
  SystemClock_Config();
  GPIO_Init();
  TIM1_PWM_Init();
  TIM3_Init();
  USART1_Init();
  motor_ready = 1U;
  motor_stop_a();
  motor_stop_b();

  print_reset_cause();
  uart_puts("speed inputs: TIM1 PWM 15 kHz, 98% duty; r=auto s=stop\r\n");
  uart_puts("auto reverse: both channels, 5s forward / 5s reverse\r\n");

  while (1) {
    uint8_t up = (HAL_GPIO_ReadPin(GPIOB, BUTTON_UP_PIN) == GPIO_PIN_RESET);
    uint8_t down = (HAL_GPIO_ReadPin(GPIOB, BUTTON_DOWN_PIN) == GPIO_PIN_RESET);

    if (HAL_UART_Receive(&huart1, &rx, 1U, 1U) == HAL_OK) {
      if (rx == '\r' || rx == '\n') {
        if (command_len != 0U) { command[command_len] = '\0'; process_command(command); command_len = 0U; }
      } else if (command_len == 0U && (rx == 'r' || rx == 'R' || rx == 'u' || rx == 'U' || rx == 'd' || rx == 'D' || rx == 's' || rx == 'S')) {
        command[0] = (char)rx; command[1] = '\0'; process_command(command);
      } else if (command_len < sizeof(command) - 1U) {
        command[command_len++] = (char)rx;
      }
    }

    /* Physical buttons command both channels and have priority over auto mode. */
    if (up || down) auto_mode = 0U;
    if (auto_mode && !up && !down) {
      /* During the pre-reversal dead time feed 0, which the stop branches below turn
       * into a brake. With REVERSE_DEAD_TIME_MS == 0 auto_braking never sets. */
      int8_t d = auto_braking ? 0 : auto_direction;
      start_motor(&a_direction, &a_remaining_ms, d, 0U);
      start_motor(&b_direction, &b_remaining_ms, d, 0U);
    } else if (up && !down) {
      start_motor(&a_direction, &a_remaining_ms, 1, 0);
      start_motor(&b_direction, &b_remaining_ms, 1, 0);
    } else if (down && !up) {
      start_motor(&a_direction, &a_remaining_ms, -1, 0);
      start_motor(&b_direction, &b_remaining_ms, -1, 0);
    } else if (up && down) {
      motor_stop_a(); motor_stop_b();
    }

    if (a_direction != 0) motor_set_a(a_direction);
    else motor_stop_a();
    if (b_direction != 0) motor_set_b(b_direction);
    else motor_stop_b();
    HAL_Delay(5U);
  }
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  osc.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV2;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}

static void GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = M1_DIR_A_PIN | M1_DIR_B_PIN | M2_DIR_A_PIN | M2_DIR_B_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP; gpio.Speed = GPIO_SPEED_FREQ_HIGH; HAL_GPIO_Init(DIR_PORT, &gpio);
  gpio.Pin = LIMIT_DOWN_PIN | LIMIT_UP_PIN | BUTTON_UP_PIN | BUTTON_DOWN_PIN;
  gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_PULLUP; HAL_GPIO_Init(GPIOB, &gpio);
  /* Reset Pull: HAL_GPIO_Init() uses it to pick the initial ODR level for output pins,
   * so leaving the pull-up from the block above in place would make the startup level
   * depend on HAL internals. The direction pins then default to ODR = 0, i.e. brake,
   * which is what we want before auto mode picks a direction. */
  /* The two speed inputs are timer outputs, not GPIOs: alternate-function push-pull,
   * driven by TIM1_CH1/CH4 from TIM1_PWM_Init(). Nothing in this program ever writes
   * these pins as GPIOs. With the timer not yet started they sit low, so the module
   * sees "no valid PWM" during startup - which, per the vendor text above, means the
   * bridge stays off. That is the safe direction for it to fail in. */
  gpio.Pin = M1_PWM_PIN | M2_PWM_PIN; gpio.Mode = GPIO_MODE_AF_PP; gpio.Speed = GPIO_SPEED_FREQ_HIGH; gpio.Pull = GPIO_NOPULL; HAL_GPIO_Init(PWM_PORT, &gpio);
  gpio.Pin = GPIO_PIN_9; gpio.Mode = GPIO_MODE_AF_PP; gpio.Speed = GPIO_SPEED_FREQ_HIGH; HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_10; gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_NOPULL; HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_13; gpio.Mode = GPIO_MODE_OUTPUT_PP; gpio.Speed = GPIO_SPEED_FREQ_LOW; HAL_GPIO_Init(GPIOC, &gpio);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

/* TIM1_CH1 (PA8) and TIM1_CH4 (PA11) drive the two HC-160A S2 speed inputs.
 * Runs free for the whole life of the program at a fixed 15 kHz / 98% duty; nothing
 * ever reprograms it. The direction pins alone decide forward / reverse / brake. */
static void TIM1_PWM_Init(void)
{
  TIM_OC_InitTypeDef oc = {0};

  __HAL_RCC_TIM1_CLK_ENABLE();
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0U;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = PWM_ARR;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0U;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) Error_Handler();

  oc.OCMode = TIM_OCMODE_PWM1;
  oc.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc.OCFastMode = TIM_OCFAST_DISABLE;
  oc.OCIdleState = TIM_OCIDLESTATE_RESET;

  oc.Pulse = PWM_CCR; /* channel 1: full speed */
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) Error_Handler();

  /* Channel 2: 0% duty when bench-testing a single motor. Parking it by stopping the
   * timer is not an option - TIM1 is shared, so that would kill channel 1 as well. */
#if CHANNEL_B_ENABLED
  oc.Pulse = PWM_CCR;
#else
  oc.Pulse = 0U;
#endif
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_4) != HAL_OK) Error_Handler();

  /* HAL_TIM_PWM_Start() also sets MOE for TIM1 (a break-capable timer); without it the
   * outputs stay disabled no matter what the compare registers say. */
  if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) Error_Handler();
  if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4) != HAL_OK) Error_Handler();
}

static void TIM3_Init(void)
{
  __HAL_RCC_TIM3_CLK_ENABLE();
  htim3.Instance = TIM3; htim3.Init.Prescaler = 6399U; htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = (64000000U / (6399U + 1U) / MOTOR_TIMER_HZ) - 1U;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK) Error_Handler();
  HAL_NVIC_SetPriority(TIM3_IRQn, 1U, 0U); HAL_NVIC_EnableIRQ(TIM3_IRQn);
  if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK) Error_Handler();
}

static void USART1_Init(void)
{
  __HAL_RCC_USART1_CLK_ENABLE();
  huart1.Instance = USART1; huart1.Init.BaudRate = 115200U; huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1; huart1.Init.Parity = UART_PARITY_NONE; huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE; huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance != TIM3) return;
  if (auto_mode) {
    if (auto_braking) {
      /* Braking phase: hold the direction pins at 0 until the dead time expires. */
      if (dead_time_remaining != 0U && --dead_time_remaining == 0U) {
        auto_braking = 0U;
        auto_direction = pending_direction;
        auto_remaining_ms = AUTO_INTERVAL_MS;
      }
    } else if (auto_remaining_ms != 0U && --auto_remaining_ms == 0U) {
      pending_direction = (auto_direction > 0) ? -1 : 1;
      if (REVERSE_DEAD_TIME_MS > 0U) {
        auto_braking = 1U;
        dead_time_remaining = REVERSE_DEAD_TIME_MS;
      } else {
        auto_direction = pending_direction;
        auto_remaining_ms = AUTO_INTERVAL_MS;
      }
    }
  }
  if (a_remaining_ms != 0U && --a_remaining_ms == 0U) a_direction = 0;
  if (b_remaining_ms != 0U && --b_remaining_ms == 0U) b_direction = 0;
}

void TIM3_IRQHandler(void) { HAL_TIM_IRQHandler(&htim3); }
void SysTick_Handler(void) { HAL_IncTick(); }
void Error_Handler(void) { if (motor_ready) { motor_stop_a(); motor_stop_b(); } __disable_irq(); while (1) {} }
