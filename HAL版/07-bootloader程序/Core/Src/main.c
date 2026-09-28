/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "Int_bootloader.h"
#include "App_bootloader.h"
#include "key.h"
#include "at24c64.h"
#include "w25q64.h"
#include "stdio.h"
#include "string.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  printf("\r\n=== boot ===\r\n");   //开机提示：每次复位（上电/点击复位）串口都会显示

  //uint32_t last_dbg = 0;   // throttle for the debug log  // 暂时注释：测试 AT24C64

  //bootloader_init();        //底层：串口接收（DMA双缓冲）+ flash 擦写，只管收   // 暂时注释：测试 AT24C64
  //App_bootloader_init();    //上层：升级状态机（等待 start:len 命令）             // 暂时注释：测试 AT24C64
  Key_Init();               //按键（PB12）状态机初始化，实际扫描在 TIM4 中断里做

  HAL_TIM_Base_Start_IT(&htim4);

  AT24C64_Init();           //必须调用：使能 DWT 周期计数器，否则软I2C的 IIC_Delay() 会死循环卡死开机
  if (W25Q64_Init() != W25Q64_OK)
  {
    printf("w25 init error\r\n");   //器件不在线/ID 不符：后面读元数据必然拿不到有效值，先报出来
  }
  App_bootloader_check_update();//检查是否需要更新

  App_bootloader_wait_key();//开机按键窗口：长按2s→默认程序(0x4000)，无按键/短按→超时跳app(0x8000)

  //是否进入默认程序，恢复出厂设置

  //App_bootloader_update();//执行更新操作（尚未实现，先注释避免链接错误）

  //App_bootloader_jump_app();//跳转到应用程序
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    //============================================================
    //  按键长按"强制结束"请求的消费点
    //  TIM4 中断里只负责置 Key_Event_Flag（见 tim.c 的 HAL_TIM_PeriodElapsedCallback），
    //  这里读取并清零后再调 app 层。分成"ISR 置标志 / 主循环消费"两步，
    //  是为了避免中断和主循环同时读写 app_bootloader_state 造成竞争。
    //============================================================
    if (Key_Event_Flag)
    {
      Key_Event_Flag = 0;
      App_bootloader_reset();      //长按按钮 → 置 BOOT_RESET（重置指令）
    }

    App_bootloader_process();      //按状态分发：BOOT_RESET → 打印reset → 跳转默认程序

    //if (HAL_GetTick() - last_dbg >= 3000)   // debug log every 3s             // 暂时注释：测试 AT24C64
    //{
    //  last_dbg = HAL_GetTick();
    //  printf("cnt:%u last:%u short:%u recv:%lu ore:%u\n",
    //         (unsigned)dbg_frame_cnt, (unsigned)dbg_last_size,
    //         (unsigned)dbg_short_size,
    //         (unsigned long)App_bootloader_recv_len(), (unsigned)dbg_ore_cnt);
    //}

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
    

  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
