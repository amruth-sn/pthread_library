#include "pthreader.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <semaphore.h>

#define EXITED 0
#define READY 1
#define RUNNING 2
#define NOSTATUS 3
#define BLOCKED 4

#define JB_RBX 0
#define JB_RBP 1
#define JB_R12 2
#define JB_R13 3
#define JB_R14 4
#define JB_R15 5
#define JB_RSP 6
#define JB_PC 7

#define MAX_THREADS 128
#define STACK_SIZE 32767
typedef struct {
        pthread_t tid;
        void *stackptr;
        jmp_buf context;
        //add jmpbuf to store state of thread, set of registers
        int status; //use define macros from above
        void * return_value;
        pthread_t blocked_thread;
} TCB_Table;
pthread_t gCurrent = 0;
static int globalThreads = 0;
TCB_Table TCB[MAX_THREADS];
int first_time = 1;

typedef struct {
        unsigned int val; //val can never be < 0
        unsigned int blocked_count; // can never have negative threads blocked
        int queue_front;
        int queue_back; //indexing queue
        int init_flag;  //sema being used
        int queue[MAX_THREADS];//statically allocate as many slots as there are possible threads, worst-case
        sem_t *current_sem; //pointer for actual sem_t type
} sema;

sema semaphores[MAX_THREADS];

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void* ), void *arg);
struct sigaction timer_handler;
void pthread_exit(void *value_ptr);
pthread_t pthread_self(void);
void init();
void scheduler();
void lock();
void unlock();
int pthread_join(pthread_t thread, void **value_ptr);
int sem_init(sem_t *sem, int pshared, unsigned value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_destroy(sem_t *sem);


int sem_init(sem_t *sem, int pshared, unsigned value){
        lock();

        int i = 1;
        while(i < MAX_THREADS){
                if(!semaphores[i++].init_flag){
                        break;
                }
                else{
                        i++;
                }
        }
        sem->__align = i; //global alignment of sem_t sem with a slot in the semaphores array
        semaphores[i].queue_front = 0;
        semaphores[i].queue_back = 0;
        semaphores[i].current_sem = sem;
        semaphores[i].val = value;
        semaphores[i].init_flag = 1; //initializing stuff ^^^

        unlock();
        return 0;
}


int sem_wait(sem_t *sem){
        lock();

        if(semaphores[sem->__align].val != 0){
                semaphores[sem->__align].val--;
                unlock();
                return 0;
        }
        else{

                TCB[gCurrent].status = BLOCKED;
                semaphores[sem->__align].queue[semaphores[sem->__align].queue_front] = TCB[gCurrent].tid;
                semaphores[sem->__align].queue_back++;
                semaphores[sem->__align].blocked_count++;
                unlock();
                return 0;
        }
}


int sem_post(sem_t *sem){
        lock();
        if(semaphores[sem->__align].blocked_count > 0){
                semaphores[sem->__align].val++;
        }

        unlock();
        return 0;
}


int sem_destroy(sem_t *sem){
        lock();
        if(semaphores[sem->__align].init_flag == 0){
                semaphores[sem->__align].blocked_count = 0;
        }

        unlock();
        return 0;
}


void lock(){
        sigset_t timer_handler_lock;
        sigset_t *lock_ptr = &timer_handler_lock;
        sigemptyset(lock_ptr);
        sigaddset(lock_ptr, SIGALRM);
        sigprocmask(SIG_BLOCK, lock_ptr, NULL);
}

void unlock(){
        sigset_t timer_handler_unlock;
        sigset_t * unlock_ptr = &timer_handler_unlock;
        sigemptyset(unlock_ptr);
        sigaddset(unlock_ptr, SIGALRM);
        sigprocmask(SIG_UNBLOCK, unlock_ptr, NULL);
}

int pthread_join(pthread_t thread, void **value_ptr){

        if(TCB[thread].status != BLOCKED && TCB[thread].status != EXITED)
{
                lock();
                TCB[gCurrent].status = BLOCKED;
                TCB[gCurrent].blocked_thread = TCB[thread].tid;
                unlock();
                scheduler();
        }

        if(value_ptr != NULL){
                lock();
                *value_ptr = TCB[thread].return_value;
                unlock();
        }

        return 0;
}

void scheduler(){
        if(TCB[gCurrent].status != EXITED && TCB[gCurrent].status != BLOCKED){
                TCB[gCurrent].status = READY;
        }

        pthread_t i = gCurrent;
        while(1){
                if(i == MAX_THREADS - 1) { i = 0; }
                else{ i++; }


                if(TCB[i].status == READY){
                        break;
                }
        }
        lock();
        int save_state = 0;
        if(TCB[gCurrent].status != EXITED){
                save_state = setjmp(TCB[gCurrent].context);
        }

        if(!save_state) {
                gCurrent = i;
                TCB[gCurrent].status = RUNNING;
                longjmp(TCB[gCurrent].context, 1);
        }

        unlock();

}

void init(){
        int i;
        for(i = 0; i < MAX_THREADS; i++){
                TCB[i].tid = -1;
                TCB[i].status = NOSTATUS; //make everything NULL/empty, will change later
                TCB[i].blocked_thread = -1;
                TCB[i].return_value = NULL;
        }
        //set up alarms/signal handler here
        __useconds_t usecs = 50000; //50000 us = 50 ms
        __useconds_t interval = 50000;
        ualarm(usecs, interval);

        sigemptyset(&timer_handler.sa_mask);
        timer_handler.sa_handler = &scheduler;
        timer_handler.sa_flags = SA_NODEFER;
        sigaction(SIGALRM, &timer_handler, NULL); //sig handler

}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void* ), void *arg){
        lock();
        attr = NULL;
        if(first_time){
                init();
                first_time = 0;
                TCB[0].status = READY;
                globalThreads++;
        }

        int i;
        for(i = 0; i < MAX_THREADS; i++){
                if(TCB[i].status == NOSTATUS){
                        break;
                }

                if(i == MAX_THREADS){
                        printf("Error, max threads reached.\n");
                        exit(1);
                }
        }

        TCB[i].tid = i;
        *thread = i;
        TCB[i].stackptr = malloc(STACK_SIZE);

        TCB[i].context[0].__jmpbuf[JB_PC] = ptr_mangle((unsigned long int) start_thunk);

        TCB[i].context[0].__jmpbuf[JB_R13] = (unsigned long int) arg;
        //Casting?
        TCB[i].context[0].__jmpbuf[JB_R12] = (unsigned long int) start_routine;
        //Casting inspired from Austin OH
        //mangling, reposition pointer
        * (unsigned long int *) ( TCB[i].stackptr + STACK_SIZE - 8) = (unsigned long int) pthread_exit_wrapper;
        void * newstackpointer = TCB[i].stackptr + STACK_SIZE - 8;
        TCB[i].context[0].__jmpbuf[JB_RSP] = ptr_mangle((unsigned long int) newstackpointer);
        TCB[i].status = READY;
        globalThreads++;
        unlock();
        scheduler();
        return 0;
}

void pthread_exit(void *value_ptr){//value_ptr is the exit value
        lock();
        TCB[gCurrent].return_value = value_ptr;
        /*if(TCB[gCurrent].blocked_thread != -1 && TCB[gCurrent].status == BLOCKED){
                TCB[TCB[gCurrent].blocked_thread].status = READY;
                TCB[gCurrent].blocked_thread = -1;      //this stuff didn't work so i reversed the logic with gCurrent and thread in pthread_join and used the for-loop up ahead to re-ready the blocked thread
        }*/
        int i;
        for(i = 0; i < MAX_THREADS; i++){
                if(TCB[i].blocked_thread == TCB[gCurrent].tid){
                        TCB[i].status = READY;
                        TCB[i].blocked_thread = -1; //came up with this corner case, so that this thread is not waiting
                }
        }

        globalThreads--;
        TCB[gCurrent].status = EXITED;

        if(TCB[gCurrent].status == EXITED){
                free(TCB[gCurrent].stackptr);
        }

        unlock();

        if(globalThreads){
                scheduler();
        }

        exit(0);
}

pthread_t pthread_self(void){
        return gCurrent;
}
