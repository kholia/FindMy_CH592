#include "CONFIG.h"
#include "HAL.h"
#include "SLEEP.h"
#include "broadcaster.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
uint8_t MacAddr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
#endif

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation()
{
    while(1)
    {
        TMOS_SystemProcess();
    }
}

int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif
#ifdef DEBUG
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    UART1_DefInit();
#endif

    Broadcaster_Prepare(MacAddr);
    CH59x_BLEInit();
    HAL_Init();
    GAPRole_BroadcasterInit();
    Broadcaster_Init();
    Main_Circulation();
}

/******************************** endfile @ main ******************************/
