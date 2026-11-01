#include "syscall.h"

USER_ENTRY
{
    static const char msg[] = "hello: Hello from user space!\n";
    sys_write(1, msg, sizeof msg - 1);
    sys_exit(0);
}
