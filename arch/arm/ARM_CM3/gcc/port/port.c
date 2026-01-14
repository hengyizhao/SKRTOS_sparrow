/*
 * MIT License
 *
 * Copyright (c) 2024 skaiui2

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *  https://github.com/skaiui2/SKRTOS_sparrow
 */

#include "port.h"
#include "config.h"

struct Stack_register {
    // manual stacking（由软件在上下文切换时保存）
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    // automatic stacking（由硬件在异常进入时自动保存）
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;   // R12通用数据寄存器，用于保存子程序调用时的链接地址
    uint32_t LR;    // 链接寄存器，用于保存返回地址
    uint32_t PC;    // 程序计数器，指向下一条要执行的指令
    uint32_t xPSR;  // 程序状态寄存器，包含异常类型、中断类型、处理器模式等信息
};


/*
 * If the program runs here, there is a problem with the use of the RTOS,
 * such as the stack allocation space is too small, and the use of undefined operations
 */
void ErrorHandle(void)
{
    while (1){

    }
}

// 所谓的栈初始化，其实就是移动任务栈顶指针，将期望的寄存器的初始化值放入任务栈中
uint32_t * StackInit( uint32_t * pxTopOfStack,
                                  TaskFunction_t pxCode,
                                  void * pvParameters)
{
    // 这里的16是保留寄存器需要的栈空间，其数值与Cortex-M3的寄存器数量一致，即Stack_register结构体的成员数量
    // 在栈顶预留16个字的空间，用于上下文切换时保存寄存器中的数据
    pxTopOfStack -= 16;
    struct Stack_register *Stack = (struct Stack_register *)pxTopOfStack;

    *Stack = (struct Stack_register) { // 任务栈初始化
        .xPSR = 0x01000000UL,
        .PC = ( ( uint32_t ) pxCode ) & ( ( uint32_t ) 0xfffffffeUL ),
        .LR = ( uint32_t ) ErrorHandle,
        .r0 = ( uint32_t ) pvParameters
    };

    return pxTopOfStack;
}


extern TaskHandle_t volatile schedule_currentTCB;
// SVC(Supervisor call)管理员调用处理程序，用于任务切换
// 触发SVC异常，会立即执行SVC_Handler函数
__attribute__( ( naked ) ) void  SVC_Handler( void )
{
    __asm volatile (
            "	ldr	r3, CurrentTCBConst2		\n"
            "	ldr r1, [r3]					\n"
            "	ldr r0, [r1]					\n"
            "	ldmia r0!, {r4-r11}				\n"
            "	msr psp, r0						\n"
            "	isb								\n"
            "	mov r0, #0 						\n"
            "	msr	basepri, r0					\n"
            "	orr r14, #0xd					\n"
            "	bx r14							\n"
            "									\n"
            "	.align 4						\n"
            "CurrentTCBConst2: .word schedule_currentTCB				\n"
            );
}

extern void TaskSwitchContext(void);
// PendSV(Pendable Supervisor call)可挂起的管理员调用处理程序，用于任务切换中断处理程序
// 在触发PendSV异常时，会执行PendSV_Handler函数
// 主要是将寄存器数据保存在旧任务的任务栈中，并从新任务的任务栈中恢复寄存器数据，从而实现任务切换
__attribute__( ( naked ) ) void PendSV_Handler( void )
{
    __asm volatile
            (
            "	mrs r0, psp							\n"
            "	isb									\n"
            "										\n"
            "	ldr	r3, CurrentTCBConst			    \n" // 获取当前任务的TCB指针，即schedule_currentTCB
            "	ldr	r2, [r3]						\n"
            "										\n"
            "	stmdb r0!, {r4-r11}					\n" // 将当前任务的寄存器保存到任务栈中
            "	str r0, [r2]						\n"
            "										\n"
            "	stmdb sp!, {r3, r14}				\n" // 保存 r3(TCB地址) 和 r14(LR) 到主栈
            "	mov r0, %0							\n" // 将屏蔽优先级值configShieldInterPriority传入 r0
            "	msr basepri, r0						\n" // 设置 BASEPRI 寄存器
            "   dsb                                 \n"
            "   isb                                 \n"
            "	bl TaskSwitchContext				\n" // 调用TaskSwitchContext()，schedule_currentTCB更新为下一个要执行的任务的TCB，因为CurrentTCBConst是schedule_currentTCB的地址，所以r3的值会更新
            "	mov r0, #0							\n"
            "	msr basepri, r0						\n"
            "	ldmia sp!, {r3, r14}				\n" // 恢复 r3(TCB地址) 和 r14(LR) 从主栈
            "										\n"
            "	ldr r1, [r3]						\n"
            "	ldr r0, [r1]						\n"
            "	ldmia r0!, {r4-r11}					\n" // 从新任务的任务栈中恢复寄存器r4-r11
            "	msr psp, r0							\n" // 更新 PSP 为新任务的栈指针
            "	isb									\n"
            "	bx r14								\n" // 异常返回，硬件自动恢复剩余寄存器
            "	nop									\n"
            "	.align 4							\n"
            "CurrentTCBConst: .word schedule_currentTCB	\n"
            ::"i" ( configShieldInterPriority )
            );
}



__attribute__((always_inline)) inline uint32_t  EnterCritical( void )
{
    uint32_t xReturn;
    uint32_t temp;

    __asm volatile(
            " cpsid i               \n"
            " mrs %0, basepri       \n"
            " mov %1, %2			\n"
            " msr basepri, %1       \n"
            " dsb                   \n"
            " isb                   \n"
            " cpsie i               \n"
            : "=r" (xReturn), "=r"(temp)
            : "r" (configShieldInterPriority)
            : "memory"
            );

    return xReturn;
}

__attribute__((always_inline)) inline void ExitCritical( uint32_t xReturn )
{
    __asm volatile(
            " cpsid i               \n"
            " msr basepri, %0       \n"
            " dsb                   \n"
            " isb                   \n"
            " cpsie i               \n"
            :: "r" (xReturn)
            : "memory"
            );
}

struct SysTicks {
    // 31      24 23      16 15      8 7       0
    // ┌─────────┬─────────┬─────────┬─────────┐
    // │  保留    │COUNTFLAG│  保留    │ ENABLE  │ SysTick 定时器使能位,0 = 定时器禁用,1 = 定时器使能
    // │         │         │         │ TICKINT │ SysTick 中断使能位,0 = 计数到0时不产生中断,1 = 计数到0时产生中断
    // │         │         │         │CLKSOURCE│ SysTick 时钟源选择位,0 = 使用外部参考时钟（STCLK）,1 = 使用处理器时钟（HCLK）
    // └─────────┴─────────┴─────────┴─────────┘
    uint32_t CTRL;
    uint32_t LOAD;  // 重装载值寄存器,写入值 = 定时周期 - 1
    uint32_t VAL;   // 当前值寄存器,读取=当前计数值写入任何值都清零，并清除 COUNTFLAG
    uint32_t CALIB; // 校准值寄存器,提供校准信息
};

 void StartFirstTask(void)
{
    // 0xe000ed20是System Control Block寄存器中的一员，用于设置系统异常优先级
    // 位31                    位23                    位15       位0
    // ├─────────────┬─────────────┬─────────────────────┤
    // │ PRI_15[31:24] │ PRI_14[23:16] │  保留[15:0] = 0    │
    // └─────────────┴─────────────┴─────────────────────┘
    // 位域	    名称	系统异常号     功能描述	         有效位
    // 31:24	PRI_15	15	        SysTick异常优先级	bit[31:28]有效，bit[27:24]读零写忽略
    // 23:16	PRI_14	14	        PendSV异常优先级	bit[23:20]有效，bit[19:16]读零写忽略
    // 15:0	    -	    -	        保留，必须保持为0	-
    // 将 SysTick（滴答定时器）和 PendSV（可挂起的系统调用）这两个系统异常的优先级设为最低（0xFF）。
    // 原因：任务切换时通过SysTick和PendSV中断触发的，确保它们不会抢占重要的硬件中断或高优先级任务，使任务切换在确定的时间点安全发生。
    // SysTick中断用于时间片触发的上下文切换，PendSV中断用于schedule()触发的上下文切换
    ( *( ( volatile uint32_t * ) 0xe000ed20 ) ) |= ( ( ( uint32_t ) 255UL ) << 16UL );
    ( *( ( volatile uint32_t * ) 0xe000ed20 ) ) |= ( ( ( uint32_t ) 255UL ) << 24UL );

    // 0xe000e010是SysTick寄存器组的基地址，用于配置和操作SysTick定时器。
    struct SysTicks *SysTick = (struct SysTicks * volatile)0xe000e010;

    /* Configure SysTick to interrupt at the requested rate. */
    *SysTick = (struct SysTicks){
            .LOAD = ( configSysTickClockHz / configTickRateHz ) - 1UL,
            .CTRL  = ( ( 1UL << 2UL ) | ( 1UL << 1UL ) | ( 1UL << 0UL ) ) // 使能SysTick定时器，使能SysTick中断
    };
    /* Start the first task. */
    __asm volatile (
            " ldr r0, =0xE000ED08 	\n"/* Use the NVIC offset register to locate the stack. */
            " ldr r0, [r0] 			\n"
            " ldr r0, [r0] 			\n"
            " msr msp, r0			\n"/* Set the msp back to the start of the stack. */
            " cpsie i				\n"/* Globally enable interrupts. */
            " cpsie f				\n"
            " dsb					\n"
            " isb					\n"
            " svc 0					\n"/* System call to start first task. 触发SVC异常，执行SVC_Handler() */
            " nop					\n"//wait
            " .ltorg				\n"
            );
}


// SysTick中断处理程序，用于处理定时器中断，执行任务切换
void SysTick_Handler(void)
{
    uint32_t xre = EnterCritical();

    CheckTicks();

    ExitCritical(xre);
}


