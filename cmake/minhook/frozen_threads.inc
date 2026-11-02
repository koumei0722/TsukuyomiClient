typedef struct _FROZEN_THREAD
{
    HANDLE hThread;
    BOOL   suspended;
    BOOL   boosted;
    int    priority;
} FROZEN_THREAD, *PFROZEN_THREAD;

typedef struct _FROZEN_THREADS
{
    PFROZEN_THREAD pItems;
    UINT    capacity;
    UINT    size;
} FROZEN_THREADS, *PFROZEN_THREADS;
