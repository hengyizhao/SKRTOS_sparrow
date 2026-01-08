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

#include "schedule.h"
#include "heap.h"
#include "port.h"


Class(TCB_t)
{
    volatile uint32_t * pxTopOfStack;
    uint8_t uxPriority;
    uint32_t * pxStack;
};


__attribute__( ( used ) )  TaskHandle_t volatile schedule_currentTCB = NULL;

TaskHandle_t GetCurrentTCB(void)
{
    return schedule_currentTCB;
}


uint8_t HighestReadyPriority = 0;

// 将最高优先级的任务设置为指定的优先级，并触发PendSV中断以切换上下文
void PreemptiveCPU(uint8_t priority)
{
    HighestReadyPriority = priority;
    schedule();
}


uint32_t  TicksBase = 0;
TaskHandle_t TcbTaskTable[configMaxPriority];

uint32_t TicksTable[configMaxPriority];
uint32_t TicksTableAssist[configMaxPriority];
uint32_t* WakeTicksTable;
uint32_t* OverWakeTicksTable;


/*
* 数组中的每一个元素对应一个task state(Dead/Ready/Delay/Suspend/Block)，每个元素的每个bit位表示一个task优先级，1表示该优先级的task处于该状态
* StateTable[Dead]      0x00000001 表示存在优先级为0的task处于Dead状态
* StateTable[Ready]     0x00000002 表示存在优先级为1的task处于Ready状态
* StateTable[Delay]     0x00000003 表示存在优先级为0和优先级为1的task处于Delay状态
* StateTable[Suspend]   0x00000004 表示存在优先级为3的task处于Suspend状态
* StateTable[Block]     0x00000005 表示存在优先级为0和优先级为3的task处于Block状态
*/
uint32_t StateTable[5] = {0,0,0,0,0};

void TcbTaskTableInit(void)
{
    for (uint8_t i = 0; i < configMaxPriority; i++) {
        TcbTaskTable[i] = NULL;
    }
}

uint8_t volatile schedule_count = 0;

// 在触发PendSV中断时，切换上下文
void TaskSwitchContext( void )
{
    schedule_count++;
    schedule_currentTCB = TcbTaskTable[HighestReadyPriority];
}

/*
 * If a higher priority task add, interrupt the current task and schedule.
 */
__attribute__((always_inline)) inline uint32_t TableAdd( TaskHandle_t taskHandle,uint8_t State) {
    uint32_t cpu_lock = EnterCritical();
    StateTable[State] |= (1 << taskHandle->uxPriority);
    if ((State == Ready) &&
        (taskHandle->uxPriority > HighestReadyPriority)) {
        HighestReadyPriority = taskHandle->uxPriority;
        schedule();
    }
    ExitCritical(cpu_lock);
    return StateTable[State];
}

__attribute__((always_inline)) inline uint32_t TableRemove( TaskHandle_t taskHandle, uint8_t State)
{
    uint32_t cpu_lock = EnterCritical();
    StateTable[State] &= ~(1 << taskHandle->uxPriority);
    if ((State == Ready) &&
        (taskHandle->uxPriority == HighestReadyPriority)) {
        HighestReadyPriority = FindHighestPriority(StateTable[Ready]);
        schedule();
    }
    ExitCritical(cpu_lock);
    return StateTable[State];
}

__attribute__((always_inline)) inline uint8_t CheckState( TaskHandle_t taskHandle,uint8_t State )// If task is the State,return true.
{

    uint32_t temp = StateTable[State];
    temp &= (1 << taskHandle->uxPriority);
    return temp;
}





/*The RTOS delay will switch the task.It is used to liberate low-priority task*/
void TaskDelay( uint16_t ticks )
{
    uint32_t WakeTime = TicksBase + ticks;
    TCB_t *self = schedule_currentTCB;
    // WakeTime = TicksBase + ticks，什么情况下会出现WakeTime < TicksBase呢，就是发生整型溢出的时候
    if (WakeTime < TicksBase) {
        OverWakeTicksTable[self->uxPriority] = WakeTime;
    }
    else {
        WakeTicksTable[self->uxPriority] = WakeTime;
    }
    StateTable[Delay] |= (1 << (self->uxPriority) );
    TableRemove(self, Ready);
}




__attribute__( ( always_inline ) ) inline uint8_t FindHighestPriority(uint32_t Table)
{
    return 31 - __builtin_clz(Table);
}


uint8_t SusPendALL = 1;
// 每一个SysTick中断调用一次
void CheckTicks(void)
{
    if (SusPendALL) {
        uint32_t LookupTable = StateTable[Delay];
        TicksBase++;
        if (TicksBase == 0) { // 用于处理TicksBase一直递增的整型数值溢出问题
            uint32_t *temp;
            temp = WakeTicksTable;
            WakeTicksTable = OverWakeTicksTable;
            OverWakeTicksTable = temp;
        }
        while (LookupTable) {
            uint8_t i = FindHighestPriority(LookupTable); // Delay表中最高优先级的任务优先被唤醒
            LookupTable &= ~(1 << i);
            if (TicksBase >= WakeTicksTable[i]) { // 唤醒的条件还有一个是休眠时刻到了
                WakeTicksTable[i] = 0; // 唤醒后将对应的唤醒时刻清零
                StateTable[Delay] &= ~(1 << i);
                TableAdd(TcbTaskTable[i], Ready); // TableAdd()中会判断优先级触发PendSV中断切换上下文
            }
        }

    }
}


void TaskSusPend(TaskHandle_t self)
{
    TableRemove(self, Ready);
    TableAdd(self, Suspend);
}

void TaskResume(TaskHandle_t self)
{
    TableRemove(self, Suspend);
    TableAdd(self, Ready);
}

void SchedulerSusPend(void)
{
    uint32_t cpu_lock = EnterCritical();
    SusPendALL = 0;
    ExitCritical(cpu_lock);
}

void SchedulerResume(void)
{
    uint32_t cpu_lock = EnterCritical();
    SusPendALL = 1;
    ExitCritical(cpu_lock);
}



void TaskCreate( TaskFunction_t pxTaskCode,
                  const uint16_t usStackDepth,
                  void * const pvParameters,
                  uint32_t uxPriority,
                  TaskHandle_t * const self )
{
    uint32_t *topStack = NULL;
    // 分配内存，用于存储任务控制块
    TCB_t *NewTcb = (TCB_t *)heap_malloc(sizeof(TCB_t));
    *self = ( TCB_t *) NewTcb;
    // 从这里可以看出该系统每个优先级只能有一个任务，如果想多个任务对应同一优先级，每个优先级应该是一个链表，链表中存放优先级的任务指针，这点在项目的README中也有说明table版本的实现就是这样的
    TcbTaskTable[uxPriority] = NewTcb;
    NewTcb->uxPriority = uxPriority;
    // 分配内存，用于任务栈
    NewTcb->pxStack = ( uint32_t *) heap_malloc( ( ( ( size_t ) usStackDepth ) * sizeof( uint32_t * ) ) );
    // 栈顶在高地址，栈底在低地址，这里需要将栈顶地址向下对齐，因为栈是向下增长的
    topStack =  NewTcb->pxStack + (usStackDepth - (uint32_t)1) ;
    topStack = ( uint32_t *) (((uint32_t)topStack) & (~((uint32_t) alignment_byte)));
    NewTcb->pxTopOfStack = StackInit(topStack,pxTaskCode,pvParameters);
    StateTable[Ready] |= (1 << uxPriority);
}

void TaskDelete(TaskHandle_t self)
{
    TableRemove(self, Ready);
    TableAdd(self, Dead);
}


void TaskFree(void)
{
    if (StateTable[Dead]) {
        TaskHandle_t self = TcbTaskTable[FindHighestPriority(StateTable[Dead])];
        TableRemove(self, Ready); // ？？？这里self是从Dead中取出的，为什么还要从Ready中移除，其应该也不在Ready中吧
        TcbTaskTable[self->uxPriority] = NULL;
        heap_free((void *)self->pxStack);
        heap_free((void *)self);
    }
}

TaskHandle_t leisureTcb = NULL;
// 空闲任务的优先级最低，其主要用于回收处于Dead状态任务的内存资源
void leisureTask( void )
{//leisureTask content can be manually modified as needed
    while (1) {
        TaskFree();
    }
}

// 任务调度函数初始化主要是初始化任务表，并且创建空闲任务
void SchedulerInit( void )
{
    TcbTaskTableInit();
    WakeTicksTable = TicksTable;
    OverWakeTicksTable = TicksTableAssist;
    TaskCreate(    (TaskFunction_t)leisureTask,
                    128,
                    NULL,
                    0, // 空闲任务的优先级是0，也就是说优先级数值越大，优先级越高
                    &leisureTcb
    );
}


void SchedulerStart( void )
{
    HighestReadyPriority = FindHighestPriority(StateTable[Ready]);
    // 获取当前最高优先级的任务，因为是先执行SchedulerInit()，如果没有添加其他的任务，此时最高优先级的任务就是空闲任务，即schedule_currentTCB = leisureTcb
    schedule_currentTCB = TcbTaskTable[HighestReadyPriority];
    StartFirstTask();
}


__attribute__((always_inline)) inline TaskHandle_t GetTaskHandle( uint8_t i)
{
    return TcbTaskTable[i];
}

__attribute__((always_inline)) inline uint8_t GetTaskPriority( TaskHandle_t taskHandle)
{
    return taskHandle->uxPriority;
}

