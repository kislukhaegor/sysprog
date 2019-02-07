#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <aio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define TASKS_DEFAULT_ALLOC_SIZE 1024
#define DEFAULT_STACK_SIZE 1024 * 64
#define handle_error(msg) \
    do {\
        perror(msg); exit(EXIT_FAILURE); \
    } while (0)


typedef enum task_state{
    TASK_READY,
    TASK_RUNNING,
    TASK_END
} task_state_t;


typedef struct task_node {
    struct task_node* next;
    struct task_node* prev;
    ucontext_t* task;
    task_state_t state;
} task_node_t;


void push_after(task_node_t* list, task_node_t* node) {
    if (!list || !node) {
        return;
    }
    if (list->next) {
        list->next->prev = node;
    }
    node->next = list->next;
    list->next = node;
    node->prev = list;
}

void push_before(task_node_t* list, task_node_t* node) {
    if (!list || !node) {
        return;
    }
    node->prev = list->prev;
    if (node->prev) {
        node->prev->next = node;
    }
    node->next = list;
    list->prev = node;
}

task_node_t* remove_node(task_node_t* node) {
    if (!node) {
        return NULL;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }

    if (node->prev) {
        node->prev->next = node->next;
    }
    node->next = NULL;
    node->prev = NULL;
    return node;
}

task_node_t* init_task_node() {
    task_node_t* node = (task_node_t*)malloc(sizeof(task_node_t));
    if (!node) {
        return NULL;
    }
    node->next = node->prev = NULL;
    node->task = (ucontext_t*)calloc(1, sizeof(ucontext_t));
    if (!node->task) {
        free(node);
        return NULL;
    }

    return node;
}

// use macro 'cause idk how to do it without __VA_ARGS__
#define add_task(task_list, routine, argc, ...) \
    do { \
        task_node_t* node = init_task_node(); \
        if (!node) { \
            break; \
        } \
        push_task(task_list, node); \
        if (getcontext(node->task) == -1) { \
            handle_error("getcontext"); \
        } \
        void *tmp_stack = allocate_task_stack(DEFAULT_STACK_SIZE); \
        if (!tmp_stack) { \
            break; \
        } \
        node->task->uc_stack.ss_sp = tmp_stack; \
        node->task->uc_stack.ss_size = DEFAULT_STACK_SIZE; \
        node->task->uc_link = &(task_list->main_context); \
        node->state = TASK_RUNNING; \
        makecontext(node->task, (void (*)(void))routine, argc + 1, task_list, __VA_ARGS__); \
    } while(0)


void free_node(task_node_t* node) {
    if (!node) {
        return;
    }
    if (node->task->uc_stack.ss_sp) {
        free(node->task->uc_stack.ss_sp);
    }
    free(node->task);
    free(node);
}


typedef struct task_list {
    task_node_t* begin;
    task_node_t* end;
    task_node_t* current_task;
    task_node_t* finished_tasks;
    size_t task_count;
    ucontext_t main_context;
} task_list_t;


task_list_t* init_task_list() {
    task_list_t* task_list = (task_list_t*)calloc(1, sizeof(task_list_t));
    memset(&(task_list->main_context), 0, sizeof(ucontext_t));
    return task_list;
}

void collect_rubbish(task_list_t* task_list) {
    if (!task_list) {
        return;
    }
    while (task_list->finished_tasks) {
        task_node_t* iter = task_list->finished_tasks;
        task_list->finished_tasks = iter->next;
        free_node(iter);
    }
}

void free_task_list(task_list_t* task_list) {
    if (!task_list) {
        return;
    }
    collect_rubbish(task_list);
    task_node_t* iter = task_list->begin;
    while (iter != NULL) {
        task_node_t* tmp = iter;
        iter = iter->next;
        free_node(tmp);
    }
    free(task_list);
}

void* allocate_task_stack(size_t stack_size) {
    void *stack = malloc(stack_size);
    stack_t ss;
    ss.ss_sp = stack;
    ss.ss_size = stack_size;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    return stack;
}

void push_task(task_list_t* task_list, task_node_t* node) {
    if (!task_list || !node) {
        return;
    }
    ++task_list->task_count;
    if (!task_list->begin && !task_list->end) {
        task_list->begin = task_list->end = node;
        return;
    }
    if (task_list->end == task_list->begin) {
        push_after(task_list->begin, node);
        task_list->end = node;
        return;
    }
    push_after(task_list->end, node);
    task_list->end = node;
}

task_node_t* remove_task(task_list_t* task_list, task_node_t* node) {
    if (!task_list || !node) {
        return node;
    }
    if (node == task_list->end) {
        task_list->end = node->prev;
        node->prev = NULL;
        if (task_list->end) {
            task_list->end->next = NULL;
        }
    }
    if (node == task_list->begin) {
        task_list->begin = node->next;
        node->next = NULL;
        if (task_list->begin) {
            task_list->begin->prev = NULL;
        }
    }
    return remove_node(node);
}

void swap_task(task_list_t* task_list) {
    task_node_t* current_task = task_list->current_task;
    if (task_list->current_task == task_list->begin) {
        task_list->current_task = task_list->end;
    } else {
        task_list->current_task = task_list->current_task->prev;
    }
    if (swapcontext(current_task->task, task_list->current_task->task) == -1) {
        handle_error("swapcontext");
    }
}

void end_task(task_list_t* task_list) {
    task_list->current_task->state = TASK_END;
    task_node_t* current_task = task_list->current_task;
    if (current_task == task_list->begin) {
        current_task = task_list->end;
    } else {
        current_task = current_task->prev;
    }
    task_node_t* finished_task = remove_task(task_list, task_list->current_task);
    if (task_list->finished_tasks) {
        push_before(task_list->finished_tasks, finished_task);
    }
    task_list->finished_tasks = finished_task;
    --task_list->task_count;
    task_list->current_task = current_task;
}

void run_tasks(task_list_t* task_list) {
    if (!task_list) {
        return;
    }
    task_list->current_task = task_list->end;
    while (task_list->task_count) {
        if (swapcontext(&task_list->main_context, task_list->current_task->task) == -1) {
            handle_error("swapcontext");
        }
        collect_rubbish(task_list);
    }
}


void routine_2(task_list_t* task_list, int id);

void routine(task_list_t* task_list, int id) {
    printf("step one; id = %d\n", id);
    if (id < 20) {
        printf("add_task id = %d; new task id = %d\n", id, id + 1);
        add_task(task_list, (void (*) (void))routine_2, 1, id + 1);
    }
    swap_task(task_list);
    printf("step two; id = %d\n", id);
    swap_task(task_list);
    printf("step three; id = %d\n", id);
    swap_task(task_list);
    end_task(task_list);
    printf("finish routine %d\n", id);
}

void routine_2(task_list_t* task_list, int id) {
    printf("Make routine_2 %d\n", id);
    swap_task(task_list);
    if (id < 20) {
        add_task(task_list, (void (*) (void))routine, 1, id + 1);
        printf("routine_2; push routine id = %d\n", id + 1);
        swap_task(task_list);
    }
    end_task(task_list);
    printf("finish_routine_2 %d\n", id);
}

void task_read_async(task_list_t* task_list, char** buf, int fd) {
    struct stat statbuf = {};
    if (fstat(fd, &statbuf) == -1) {
        return;
    }
    swap_task(task_list);
    
    *buf = (char*)malloc(sizeof(char) * statbuf.st_size);
    if (!(*buf)) {
        return;
    }

    swap_task(task_list);

    struct aiocb cb = {
        .aio_fildes = fd,
        .aio_buf = *buf,
        .aio_offset = lseek(fd, 0, SEEK_CUR),
        .aio_nbytes = statbuf.st_size,
    };
    aio_read(&cb);
    swap_task(task_list);

    int ret_code = 0;
    while ((ret_code = aio_error(&cb)) == EINPROGRESS) {
        swap_task(task_list);
    }
    int result = aio_return(&cb);
    printf("result %d = \n", result);
    end_task(task_list);
}

#define SIZE 1024
int main(int argc, char* argv[]) {
    if (argc < 2) {
        return -1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        return -1;
    }



    char buf[SIZE * 3] = {};

    task_list_t* task_list = init_task_list();
    if (!task_list) {
        return 0;
    }
    for (int i = 0; i < 3; ++i) {
        add_task(task_list, (void (*) (void))task_read_async, 4, buf + SIZE * i, fd, SIZE * i, SIZE);
    }
    run_tasks(task_list);
    free_task_list(task_list);
    printf("%s\n", buf);
    printf("End tasks\n");
    close(fd);
}