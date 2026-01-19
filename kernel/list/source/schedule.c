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


#include <memory.h>
#include "schedule.h"
#include "heap.h"
#include "list.h"
#include "port.h"


Class(TCB_t)
{
    volatile uint32_t *pxTopOfStack;
    ListNode task_node; // 其value用于存储为该任务设置的时间片，即TimeSlice
    ListNode IPC_node; // 其value用于存储为该任务设置的优先级
    uint8_t state;
    uint8_t uxPriority;
    uint32_t * pxStack;
    uint8_t TimeSlice;
};

__attribute__( ( used ) )  TaskHandle_t volatile schedule_currentTCB = NULL;

__attribute__( ( always_inline ) ) inline TaskHandle_t GetCurrentTCB(void)
{
    return schedule_currentTCB;
}


__attribute__( ( always_inline ) ) inline TaskHandle_t TaskHighestPriorityTask(TheList *xlist)
{
    return container_of(xlist->tail, TCB_t, task_node); // 因为优先级数值越大，表示优先级越高，所以这里使用的是xlist->tail来获取优先级最高的任务
}

__attribute__( ( always_inline ) ) inline TaskHandle_t IPCHighestPriorityTask(TheList *xlist)
{
    return container_of(xlist->tail, TCB_t, IPC_node);
}

uint8_t GetTaskPriority(TaskHandle_t taskHandle)
{
    return taskHandle->uxPriority;
}



TheList ReadyListArray[configMaxPriority]; // 每个优先级分配一个ReadyList，其SaceNode用于存储同优先级时间片轮转的任务，SwitchFlag用于存储时间片轮转的计时值
TheList OneDelayList;
TheList TwoDelayList;
TheList *WakeTicksList;
TheList *OverWakeTicksList;

static volatile uint32_t NowTickCount = ( uint32_t ) 0;

TheList SuspendList;
TheList BlockList;
TheList DeleteList;

static void ReadyListInit( void )
{
    uint8_t i = 0;
    while( i < configMaxPriority)
    {
        ListInit(&(ReadyListArray[i]));
        i++;
    }
}



static void ReadyListAdd(ListNode *node)
{
    TaskHandle_t self = container_of(node, TCB_t, task_node);
    self->task_node.value = self->TimeSlice;
    ListAdd( &(ReadyListArray[self->uxPriority]), node);
}


static void ReadyListRemove(ListNode *node)
{
    TaskHandle_t self = container_of(node, TCB_t, task_node);
    ListRemove( &(ReadyListArray[self->uxPriority]), node);
}

static void SuspendListAdd(ListNode *node)
{
    ListAdd( &SuspendList, node);
}

static void SuspendListRemove(ListNode *node)
{
    ListRemove( &SuspendList, node);
}


void TaskListAdd(TaskHandle_t self, uint8_t State)
{
    uint32_t xReturn = xEnterCritical();
    ListNode *node = &(self->task_node);
    void (*ListAdd[])(ListNode *node) = {
            ReadyListAdd,
            SuspendListAdd
    };
    ListAdd[State](node);
    xExitCritical(xReturn);
}



void TaskListRemove(TaskHandle_t self, uint8_t State)
{
    uint32_t xReturn = xEnterCritical();
    ListNode *node = &(self->task_node);
    void (*ListRemove[])(ListNode *node) = {
            ReadyListRemove,
            SuspendListRemove
    };
    ListRemove[State](node);
    xExitCritical(xReturn);
}

void Insert_IPC(TaskHandle_t self,TheList *IPC_list)
{
    self->IPC_node.value = self->uxPriority;
    ListAdd( IPC_list , &(self->IPC_node));
}


void Remove_IPC(TaskHandle_t self)
{
    ListRemove( self->IPC_node.TheList , &(self->IPC_node));
}

static uint8_t ListHighestPriorityTask(void)
{
    uint8_t i = configMaxPriority - 1;
    // 这里没有对i的值进行判断，因为默认至少任务列表中中包括空闲任务的，其不会出现i一直递减，导致无符号整型溢出的风险
    while(i > 0) {
        if (ReadyListArray[i].count > 0) {
            return i;
        }
        i--;
    }
    return 0;
}


void ADTListInit(void)
{
    ReadyListInit();
    ListInit( &SuspendList );
    ListInit( &BlockList );
    ListInit( &DeleteList );
}

__attribute__((always_inline)) inline void StateSet( TaskHandle_t taskHandle,uint8_t State)
{
    taskHandle->state = State;
}

__attribute__((always_inline)) inline uint8_t CheckIPCState( TaskHandle_t taskHandle)
{
    return taskHandle->IPC_node.TheList == NULL;
}

__attribute__((always_inline)) inline uint8_t CheckTaskState( TaskHandle_t taskHandle, uint8_t State)// If task is the State,return true
{
    return taskHandle->state == State;
}


uint8_t volatile schedule_PendSV = 0;
// TaskSwitchContext()的调用一处是在SchedulerStart()调度器启动，一处是在PendSV_Handler()的中断处理程序中
// 在schedulerStart()调度器启动的时候调用TaskSwitchContext()不需要考虑多任务的同步
// 在PendSV_Handler()的中断处理程序中，进行了中断的屏蔽，所以TaskSwitchContext()中不需要做相关的任务互斥同步逻辑
void TaskSwitchContext(void)
{
    uint8_t Index= ListHighestPriorityTask();
    TheList *TopPrioritiesList = &(ReadyListArray[Index]);
    if( TopPrioritiesList->SwitchFlag > 0) {
        TopPrioritiesList->SwitchFlag -= 1;
    } else {
        TopPrioritiesList->SaveNode = TopPrioritiesList->SaveNode->next;
        TopPrioritiesList->SwitchFlag = TopPrioritiesList->SaveNode->value;
    }

    schedule_PendSV++;
    schedule_currentTCB = container_of(TopPrioritiesList->SaveNode,TCB_t ,task_node);
}


void RecordWakeTime(uint16_t ticks)
{
    const uint32_t constTicks = NowTickCount;
    TCB_t *self = schedule_currentTCB;
    const uint32_t wakeTime = constTicks + ticks;

    if(wakeTime < constTicks) {
        ListAdd(OverWakeTicksList, &(self->task_node));
    } else {
        ListAdd(WakeTicksList, &(self->task_node));
    }
}


/*The RTOS delay will switch the task.It is used to liberate low-priority task*/
void TaskDelay( uint16_t ticks )
{
    TaskListRemove(schedule_currentTCB,Ready);
    RecordWakeTime(ticks);
    schedule();
}


void TaskCreate( TaskFunction_t pxTaskCode,
                  const uint16_t usStackDepth,
                  void * const pvParameters,//You can use it for debugging
                  uint32_t uxPriority,
                  TaskHandle_t * const self,
                  uint8_t TimeSlice)
{
    uint32_t *topStack = NULL;
    uint32_t *pxStack = ( uint32_t *) heap_malloc( ( ( ( size_t ) usStackDepth ) * sizeof( uint32_t * ) ) );
    TCB_t *NewTcb = (TCB_t *)heap_malloc(sizeof(TCB_t));
    memset( ( void * ) NewTcb, 0x00, sizeof( TCB_t ) );
    *self = ( TCB_t *) NewTcb;
    *NewTcb = (TCB_t){
        .state = Ready,
        .uxPriority = uxPriority,
        .TimeSlice = TimeSlice,
        .pxStack = pxStack
    };
    ListNodeInit(&NewTcb->task_node);
    ListNodeInit(&NewTcb->IPC_node);
    topStack =  NewTcb->pxStack + (usStackDepth - (uint32_t)1) ;
    topStack = ( uint32_t *) (((uint32_t)topStack) & (~((uint32_t) alignment_byte)));
    NewTcb->pxTopOfStack = pxPortInitialiseStack(topStack,pxTaskCode,pvParameters);
    TaskListAdd(NewTcb, Ready);
}

void TaskDelete(TaskHandle_t self)
{
    TaskListRemove(self, Ready);
    ListAdd(&DeleteList, &self->task_node);
    schedule();
}


void ListDelayInit(void)
{
    ListInit( &OneDelayList );
    ListInit( &TwoDelayList);
    WakeTicksList = &OneDelayList;
    OverWakeTicksList = &TwoDelayList;
}


void TaskFree(void)
{
    if (DeleteList.count != 0) {
        TaskHandle_t self = container_of(DeleteList.head, TCB_t, task_node);
        ListRemove(&DeleteList, &self->task_node);
        heap_free((void *)self->pxStack);
        heap_free((void *)self);
    }
}

//Task handle can be hide, but in order to debug, it must be created manually by the user
TaskHandle_t leisureTcb = NULL;

void leisureTask( void )
{//leisureTask content can be manually modified as needed
    while (1) {
        TaskFree();
    }
}

void LeisureTaskCreat(void)
{
    TaskCreate(    (TaskFunction_t)leisureTask,
                    128,
                    NULL,
                    0,
                    &leisureTcb,
                    0 // 空闲任务的时间片设置为0，也是可以调度到的，TaskSwitchContext()中，如果优先级0中的其他任务switchFlag<0之后就会调度到空闲任务，即使优先级0只有空闲任务，每次也是可以调度到空闲任务的
    );
}


void SchedulerInit(void)
{
    ADTListInit();
    ListDelayInit();
    LeisureTaskCreat();
}

__attribute__( ( always_inline ) )  inline void SchedulerStart( void )
{
    TaskSwitchContext(); // 在调度器启动时，执行TaskSwitchContext()后，schedule_PendSV = 1， schedule_currentTCB = 空闲任务
    StartFirstTask();
}


void DelayListRemove(TaskHandle_t self)
{
    ListRemove(WakeTicksList, &(self->task_node));
}


void CheckTicks(void)
{
    ListNode *list_node = NULL;
    NowTickCount++;

    if( NowTickCount == ( uint32_t) 0UL) {
        TheList *temp;
        temp = WakeTicksList;
        WakeTicksList = OverWakeTicksList;
        OverWakeTicksList = temp;
    }

    while ( (list_node = WakeTicksList->head) && (list_node->value <= NowTickCount) ) {
        TaskHandle_t self = container_of(list_node, TCB_t, task_node);
        DelayListRemove(self);
        TaskListAdd(self, Ready);
    }

    schedule(); // 触发PendSV中断
}
