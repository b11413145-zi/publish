#include "stm32g474xx.h"
#include <string.h>

// ----------------------------------------------------------------------------
// 狀態與全域變數定義
// ----------------------------------------------------------------------------
typedef enum {
    STATE_IDLE,     // 待機模式
    STATE_FOCUS,    // 專注模式 (深呼吸燈)
    STATE_PAUSE,    // 暫停模式 (凍結)
    STATE_BREAK,    // 休息模式 (輕快雙閃)
    STATE_GRACE,    // ⚡ 新增：休息結束後的 5 秒緩衝寬限期
    STATE_PUNISH    // 懲罰模式 (超時偷懶爆閃 + 高頻警報)
} SystemState_t;

volatile SystemState_t current_state = STATE_IDLE;
volatile SystemState_t last_state = STATE_FOCUS;
volatile uint16_t countdown_seconds = 0;
volatile uint16_t punish_seconds = 0;    // 懲罰超時計數
volatile uint32_t button_tick = 0;       // 按鍵防彈跳

// Ring Buffer (UART 接收)
#define RING_BUF_SIZE 64
volatile char ring_buffer[RING_BUF_SIZE];
volatile uint8_t head = 0;
volatile uint8_t tail = 0;

// PWM 呼吸燈控制變數
volatile uint16_t pwm_val = 0;
volatile int16_t pwm_step = 40;
volatile uint8_t breath_dir = 1;
volatile uint16_t dark_pause_cnt = 0;

// 蜂鳴器控制變數
volatile uint16_t buzzer_timer = 0;

// ----------------------------------------------------------------------------
// 函式宣告
// ----------------------------------------------------------------------------
void SystemClock_Config(void);
void GPIO_Init(void);
void TIM2_PWM_Init(void);
void TIM3_1s_Init(void);
void TIM4_Buzzer_Init(void);
void EXTI15_10_Init(void);
void USART2_Init(void);
void UART_SendString(const char* str);
void Set_Buzzer_Freq(uint32_t freq_hz, uint8_t enable);
void Process_Command(const char* cmd);

// ----------------------------------------------------------------------------
// 主程式 Main
// ----------------------------------------------------------------------------
int main(void) {
    SystemClock_Config();
    GPIO_Init();
    USART2_Init();
    TIM2_PWM_Init();
    TIM3_1s_Init();
    TIM4_Buzzer_Init();
    EXTI15_10_Init();

    for(volatile int i = 0; i < 200000; i++); // 開機延遲

    UART_SendString("\r\n==== Punishing Pomodoro Initialized ====\r\n");
    UART_SendString("Commands: START, STOP, PAUSE, TEST\r\n");

    char cmd_line[32];
    uint8_t cmd_idx = 0;

    while (1) {
        while (head != tail) {
            char ch = ring_buffer[tail];
            tail = (tail + 1) % RING_BUF_SIZE;

            if (ch == '\r' || ch == '\n') {
                if (cmd_idx > 0) {
                    cmd_line[cmd_idx] = '\0';
                    Process_Command(cmd_line);
                    cmd_idx = 0;
                }
            } else if (cmd_idx < sizeof(cmd_line) - 1) {
                cmd_line[cmd_idx++] = ch;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// UART 指令解析邏輯
// ----------------------------------------------------------------------------
void Process_Command(const char* cmd) {
    if (strstr(cmd, "START") || strstr(cmd, "start")) {
        current_state = STATE_FOCUS;
        countdown_seconds = 1500; // 25 分鐘
        dark_pause_cnt = 0;
        pwm_val = 0;
        breath_dir = 1;
        Set_Buzzer_Freq(0, 0);
        UART_SendString("\r\n[CMD] Focus Started (25 Mins)\r\n");
    }
    else if (strstr(cmd, "STOP") || strstr(cmd, "stop")) {
        current_state = STATE_IDLE;
        countdown_seconds = 0;
        Set_Buzzer_Freq(0, 0);
        TIM2->CCR1 = 0;
        UART_SendString("\r\n[CMD] System Stopped -> IDLE Mode\r\n");
    }
    else if (strstr(cmd, "PAUSE") || strstr(cmd, "pause")) {
        if (current_state == STATE_FOCUS) {
            last_state = current_state;
            current_state = STATE_PAUSE;
            UART_SendString("\r\n[CMD] Paused (Frozen)\r\n");
        } else if (current_state == STATE_PAUSE) {
            current_state = last_state;
            UART_SendString("\r\n[CMD] Resumed Focus!\r\n");
        }
    }
    else if (strstr(cmd, "TEST") || strstr(cmd, "test")) {
        current_state = STATE_FOCUS;
        countdown_seconds = 5; // 5秒專注 -> 5秒休息 -> 5秒寬限期 -> 懲罰
        dark_pause_cnt = 0;
        pwm_val = 0;
        breath_dir = 1;
        Set_Buzzer_Freq(0, 0);
        UART_SendString("\r\n[TEST] Focus 5s -> Break 5s -> Grace 5s -> Punish Test Started!\r\n");
    }
}

// ----------------------------------------------------------------------------
// TIM2：PWM 與燈效控制 (10ms 觸發一次)
// ----------------------------------------------------------------------------
void TIM2_IRQHandler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;

        static uint16_t double_flash_cnt = 0;
        static uint8_t grace_flash_cnt = 0;
        static uint8_t punish_flash_cnt = 0;

        if (current_state == STATE_FOCUS) {
            if (dark_pause_cnt > 0) {
                dark_pause_cnt--;
                TIM2->CCR1 = 0;
            } else {
                if (breath_dir) {
                    pwm_val += pwm_step;
                    if (pwm_val >= 9600) {
                        pwm_val = 9600;
                        breath_dir = 0;
                    }
                } else {
                    if (pwm_val <= pwm_step) {
                        pwm_val = 0;
                        breath_dir = 1;
                        dark_pause_cnt = 100;
                    } else {
                        pwm_val -= pwm_step;
                    }
                }
                TIM2->CCR1 = pwm_val;
            }
        }
        else if (current_state == STATE_PAUSE) {
            TIM2->CCR1 = pwm_val;
        }
        else if (current_state == STATE_BREAK) {
            // 休息模式：輕快雙閃
            double_flash_cnt = (double_flash_cnt + 1) % 120;
            if (double_flash_cnt < 10 || (double_flash_cnt >= 25 && double_flash_cnt < 35)) {
                TIM2->CCR1 = 8000;
            } else {
                TIM2->CCR1 = 0;
            }
        }
        else if (current_state == STATE_GRACE) {
            // 💡 寬限期燈效：中等亮度慢閃 (每 200ms 切換一次)
            grace_flash_cnt++;
            if (grace_flash_cnt >= 20) {
                grace_flash_cnt = 0;
                TIM2->CCR1 = (TIM2->CCR1 == 0) ? 5000 : 0;
            }
        }
        else if (current_state == STATE_PUNISH) {
            // 🚨 懲罰模式：極速爆閃
            punish_flash_cnt++;
            if (punish_flash_cnt >= 5) {
                punish_flash_cnt = 0;
                TIM2->CCR1 = (TIM2->CCR1 == 0) ? 9900 : 0;
            }
        }
        else {
            TIM2->CCR1 = 0;
        }
    }
}

// ----------------------------------------------------------------------------
// TIM3：1 秒精準倒數中斷
// ----------------------------------------------------------------------------
void TIM3_IRQHandler(void) {
    if (TIM3->SR & TIM_SR_UIF) {
        TIM3->SR &= ~TIM_SR_UIF;

        if (button_tick > 0) button_tick--;

        if (buzzer_timer > 0) {
            buzzer_timer--;
            if (buzzer_timer == 0 && current_state != STATE_PUNISH) {
                Set_Buzzer_Freq(0, 0);
            }
        }

        if (current_state == STATE_PAUSE) return;

        if (current_state == STATE_PUNISH) {
            punish_seconds++;
            if (punish_seconds % 2 == 0) {
                Set_Buzzer_Freq(3500, 1);
            } else {
                Set_Buzzer_Freq(2000, 1);
            }
            return;
        }

        if (countdown_seconds > 0) {
            countdown_seconds--;
        } else {
            // 時間歸零邏輯
            if (current_state == STATE_FOCUS) {
                current_state = STATE_BREAK;
                countdown_seconds = 5;    // 休息 5 秒
                Set_Buzzer_Freq(1500, 1);
                buzzer_timer = 1;
                UART_SendString("\r\n[STATUS] Focus Done! Enter Break Mode.\r\n");
            }
            else if (current_state == STATE_BREAK) {
                // 💡 休息結束：進入 5 秒緩衝寬限期 (STATE_GRACE)
                current_state = STATE_GRACE;
                countdown_seconds = 5;    // 給你 5 秒按按鈕
                Set_Buzzer_Freq(2000, 1);
                buzzer_timer = 1;
                UART_SendString("\r\n[GRACE] Break Over! Press button within 5s or get punished!\r\n");
            }
            else if (current_state == STATE_GRACE) {
                // 💡 緩衝期也用完未按按鈕 -> 進入懲罰模式！
                current_state = STATE_PUNISH;
                punish_seconds = 0;
                Set_Buzzer_Freq(3000, 1);
                UART_SendString("\r\n[PUNISH] Grace Time Expired! Get Punished!\r\n");
            }
        }
    }
}

// ----------------------------------------------------------------------------
// EXTI15_10：PC13 藍色按鈕
// ----------------------------------------------------------------------------
void EXTI15_10_IRQHandler(void) {
    if (EXTI->PR1 & EXTI_PR1_PIF13) {
        EXTI->PR1 = EXTI_PR1_PIF13;

        if (button_tick == 0) {
            button_tick = 1;

            // 💡 在待機、懲罰中、或寬限期按下按鈕 -> 都能順利回到專注模式！
            if (current_state == STATE_IDLE || current_state == STATE_PUNISH || current_state == STATE_GRACE) {
                current_state = STATE_FOCUS;
                countdown_seconds = 1500;
                dark_pause_cnt = 0;
                pwm_val = 0;
                breath_dir = 1;
                Set_Buzzer_Freq(0, 0);
                UART_SendString("\r\n[Button] Resumed to Focus Started!\r\n");
            }
            else if (current_state == STATE_FOCUS) {
                last_state = STATE_FOCUS;
                current_state = STATE_PAUSE;
                UART_SendString("\r\n[Button] Paused (Frozen)\r\n");
            }
            else if (current_state == STATE_PAUSE) {
                current_state = last_state;
                UART_SendString("\r\n[Button] Resumed!\r\n");
            }
            else if (current_state == STATE_BREAK) {
                current_state = STATE_FOCUS;
                countdown_seconds = 1500;
                dark_pause_cnt = 0;
                Set_Buzzer_Freq(0, 0);
                UART_SendString("\r\n[Button] Skip Break, Focus Started!\r\n");
            }
        }
    }
}

// ----------------------------------------------------------------------------
// USART2 中斷服務程式
// ----------------------------------------------------------------------------
void USART2_IRQHandler(void) {
    if (USART2->ISR & USART_ISR_RXNE_RXFNE) {
        char ch = (char)(USART2->RDR);
        uint8_t next_head = (head + 1) % RING_BUF_SIZE;
        if (next_head != tail) {
            ring_buffer[head] = ch;
            head = next_head;
        }
    }
}

// ----------------------------------------------------------------------------
// 硬體暫存器初始化
// ----------------------------------------------------------------------------
void SystemClock_Config(void) {
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));
    RCC->CCIPR &= ~RCC_CCIPR_USART2SEL_Msk;
}

void GPIO_Init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;

    GPIOA->MODER &= ~GPIO_MODER_MODE5_Msk;
    GPIOA->MODER |= (2 << GPIO_MODER_MODE5_Pos);
    GPIOA->AFR[0] &= ~GPIO_AFRL_AFSEL5_Msk;
    GPIOA->AFR[0] |= (1 << GPIO_AFRL_AFSEL5_Pos);

    GPIOB->MODER &= ~GPIO_MODER_MODE6_Msk;
    GPIOB->MODER |= (2 << GPIO_MODER_MODE6_Pos);
    GPIOB->AFR[0] &= ~GPIO_AFRL_AFSEL6_Msk;
    GPIOB->AFR[0] |= (2 << GPIO_AFRL_AFSEL6_Pos);

    GPIOC->MODER &= ~GPIO_MODER_MODE13_Msk;
    GPIOC->PUPDR &= ~GPIO_PUPDR_PUPD13_Msk;
}

void TIM2_PWM_Init(void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
    TIM2->PSC = 16 - 1;
    TIM2->ARR = 10000 - 1;
    TIM2->CCR1 = 0;
    TIM2->CCMR1 &= ~(0xFF);
    TIM2->CCMR1 |= (6 << 4);
    TIM2->CCER |= TIM_CCER_CC1E;
    TIM2->DIER |= TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 |= TIM_CR1_CEN;
}

void TIM3_1s_Init(void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM3EN;
    TIM3->PSC = 16000 - 1;
    TIM3->ARR = 1000 - 1;
    TIM3->DIER |= TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CR1 |= TIM_CR1_CEN;
}

void TIM4_Buzzer_Init(void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM4EN;
    TIM4->PSC = 16 - 1;
    TIM4->ARR = 1000 - 1;
    TIM4->CCR1 = 0;
    TIM4->CCMR1 &= ~(0xFF);
    TIM4->CCMR1 |= (6 << 4);
    TIM4->CCER |= TIM_CCER_CC1E;
    TIM4->CR1 |= TIM_CR1_CEN;
}

void Set_Buzzer_Freq(uint32_t freq_hz, uint8_t enable) {
    if (!enable || freq_hz == 0) {
        TIM4->CCR1 = 0;
        return;
    }
    uint32_t arr_val = 1000000 / freq_hz;
    TIM4->ARR = arr_val - 1;
    TIM4->CCR1 = arr_val / 2;
}

void EXTI15_10_Init(void) {
    RCC->APB2ENR |= RCC->APB2ENR | RCC_APB2ENR_SYSCFGEN; // 修正語法
    SYSCFG->EXTICR[3] &= ~SYSCFG_EXTICR4_EXTI13_Msk;
    SYSCFG->EXTICR[3] |= SYSCFG_EXTICR4_EXTI13_PC;
    EXTI->FTSR1 |= EXTI_FTSR1_FT13;
    EXTI->RTSR1 &= ~EXTI_RTSR1_RT13;
    EXTI->IMR1 |= EXTI_IMR1_IM13;
    EXTI->PR1 = EXTI_PR1_PIF13;
    NVIC_SetPriority(EXTI15_10_IRQn, 2);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void USART2_Init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    GPIOA->MODER &= ~(GPIO_MODER_MODE2_Msk | GPIO_MODER_MODE3_Msk);
    GPIOA->MODER |= (2 << GPIO_MODER_MODE2_Pos) | (2 << GPIO_MODER_MODE3_Pos);
    GPIOA->AFR[0] &= ~(GPIO_AFRL_AFSEL2_Msk | GPIO_AFRL_AFSEL3_Msk);
    GPIOA->AFR[0] |= (7 << GPIO_AFRL_AFSEL2_Pos) | (7 << GPIO_AFRL_AFSEL3_Pos);
    USART2->BRR = 139;
    USART2->CR1 |= USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_EnableIRQ(USART2_IRQn);
    USART2->CR1 |= USART_CR1_UE;
}

void UART_SendString(const char* str) {
    while (*str) {
        while (!(USART2->ISR & USART_ISR_TXE_TXFNF));
        USART2->TDR = *str++;
    }
}
