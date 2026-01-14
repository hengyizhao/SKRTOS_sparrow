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

#include "RWlock.h"
#include "sem.h"
#include "heap.h"


/*
 * many reader, many writer.
 *
 */

Class(rwlock)
{
    Semaphore_Handle read;
    Semaphore_Handle write;
    Semaphore_Handle W_guard; // 0-1信号量，用于保护写者互斥访问，write_acquire时获取，write_release时释放
    Semaphore_Handle C_guard; // 0-1信号量，用于保护计数器互斥访问，在访问计数器时获取，访问计数器结束后释放
    int active_reader;
    int reading_reader;
    int active_writer; // 请求写的任务数量
    int writing_writer; // 等待获取W_guard信号量的任务数量
};

rwlock_handle rwlock_creat(void)
{
    rwlock_handle rwlock1 = heap_malloc(sizeof (rwlock));
    *rwlock1 = (rwlock){
            .read = semaphore_creat(0),
            .write = semaphore_creat(0),
            .W_guard = semaphore_creat(1),
            .C_guard = semaphore_creat(1),
            .active_reader = 0,
            .reading_reader = 0,
            .active_writer = 0,
            .writing_writer = 0
    };
    return rwlock1;
}

void read_acquire(rwlock_handle rwlock1)
{
    // 缺少信号量获取的结果检查，获取到了信号量才能继续执行
    // 如果没有获取到信号量，肯定不能去修改计数器的数据
    semaphore_take(rwlock1->C_guard, 1);
    rwlock1->active_reader += 1;
    if(rwlock1->active_writer == 0){
        rwlock1->reading_reader += 1;
        semaphore_release(rwlock1->read);
    }
    semaphore_release(rwlock1->C_guard);
    // 解决write与read的互斥操作，两种情况：
    // 1、没有其他任务在等待write：rwlock1->active_writer == 0，那么执行semaphore_release(rwlock1->read)，这时可以获取read信号量
    // 2、有其他任务在等待write：rwlock1->active_writer != 0，没有执行semaphore_release(rwlock1->read)，那么不能获取read信号量，任务就会被阻塞
    //      此时需要等待write的任务执行write_release()时，执行semaphore_release(rwlock1->read)，然后唤醒阻塞的任务
    semaphore_take(rwlock1->read, 1);
}

void read_release(rwlock_handle rwlock1)
{
    semaphore_take(rwlock1->C_guard, 1);
    rwlock1->reading_reader -= 1;
    rwlock1->active_reader -= 1;
    // 解决write与read的互斥操作中的情况2，循环唤醒所有在等待获取write信号量的任务
    if(rwlock1->reading_reader == 0){
        while(rwlock1->writing_writer < rwlock1->active_writer){
            rwlock1->writing_writer += 1;
            semaphore_release(rwlock1->write);
        }
    }
    semaphore_release(rwlock1->C_guard);
}

void write_acquire(rwlock_handle rwlock1)
{
    semaphore_take(rwlock1->C_guard, 1);
    rwlock1->active_writer += 1;
    if(rwlock1->reading_reader == 0){
        rwlock1->writing_writer += 1;
        semaphore_release(rwlock1->write);
    }
    semaphore_release(rwlock1->C_guard);
    // 解决write与read的互斥操作，两种情况：
    // 1、没有其他任务正在reading，直接获取write信号量：rwlock1->reading_reader == 0，那么执行semaphore_release(rwlock1->write)，这时可以获取write信号量
    // 2、有其他任务正在reading，无法获取write信号量：
    //      rwlock1->reading_reader != 0，没有执行semaphore_release(rwlock1->write)，那么不能获取write信号量，任务就会被阻塞
    //      此时需要等待read的任务执行read_release()时，执行semaphore_release(rwlock1->write)，然后唤醒阻塞的任务
    semaphore_take(rwlock1->write, 1);

    // 解决write的互斥操作，两种情况：
    // 1、没有其他任务正在writing，直接获取W_guard信号量：W_guard信号量初始值为1，这时可以获取W_guard信号量
    // 2、有其他任务正在writing，无法获取W_guard信号量：
    //      不能获取W_guard信号量，任务被阻塞，此时需要等待write的任务执行write_release()时，执行semaphore_release(rwlock1->W_guard)，然后唤醒阻塞的任务
    semaphore_take(rwlock1->W_guard, 1);
}


void write_release(rwlock_handle rwlock1)
{
    semaphore_release(rwlock1->W_guard);

    semaphore_take(rwlock1->C_guard, 1);
    rwlock1->writing_writer -= 1;
    rwlock1->active_writer -= 1;
    // 解决write与read的互斥操作中的情况2，循环唤醒所有在等待获取read信号量的任务
    if(rwlock1->active_writer == 0){
        while(rwlock1->reading_reader < rwlock1->active_reader){
            rwlock1->reading_reader += 1;
            semaphore_release(rwlock1->read);
        }
    }
    semaphore_release(rwlock1->C_guard);
}

void rwlock_delete(rwlock_handle rwlock1)
{
    heap_free(rwlock1);
}
