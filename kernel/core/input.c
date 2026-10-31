#include <miga/input.h>

static volatile char q[256];
static volatile unsigned head, tail;

#define MAX_POLLERS 4
static input_poll_fn pollers[MAX_POLLERS];

void input_push(char c)
{
    unsigned n = (head + 1) & 255;
    if (n == tail) return; /* очередь полна: символ теряется */
    q[head] = c;
    head = n;
}

int input_register_poller(input_poll_fn fn)
{
    for (int i = 0; i < MAX_POLLERS; i++)
        if (pollers[i] == fn) return 0;                  /* несколько клавиатур/мышей одного драйвера */
    for (int i = 0; i < MAX_POLLERS; i++)
        if (!pollers[i]) { pollers[i] = fn; return 0; }
    return -1;
}

void input_poll(void)
{
    for (int i = 0; i < MAX_POLLERS && pollers[i]; i++) pollers[i]();
}

int input_getc(void)
{
    input_poll();
    if (head == tail) return -1;
    char c = q[tail];
    tail = (tail + 1) & 255;
    return (unsigned char)c;
}

/* ------------------------------------------------------------------ мышь */
#define MQ 512                                           /* 1000 пакетов/с * ~0,5 с запаса */
static volatile struct mouse_pkt mq[MQ];
static volatile unsigned mhead, mtail;
static const char *mkinds[4];
static unsigned nmice;

void mouse_push(int dx, int dy, int wheel, unsigned buttons)
{
    unsigned n = (mhead + 1) % MQ;
    if (n == mtail) {                                    /* очередь полна: сливаем с последним пакетом, а не теряем движение */
        volatile struct mouse_pkt *l = &mq[(mhead + MQ - 1) % MQ];
        l->dx = (int16_t)(l->dx + dx); l->dy = (int16_t)(l->dy + dy);
        l->wheel = (int8_t)(l->wheel + wheel); l->buttons = (uint8_t)buttons;
        return;
    }
    if (dx > 32767) dx = 32767;
    if (dx < -32768) dx = -32768;
    if (dy > 32767) dy = 32767;
    if (dy < -32768) dy = -32768;
    mq[mhead].dx = (int16_t)dx; mq[mhead].dy = (int16_t)dy;
    mq[mhead].wheel = (int8_t)wheel; mq[mhead].buttons = (uint8_t)buttons;
    mhead = n;
}

int mouse_pop(struct mouse_pkt *out)
{
    if (mhead == mtail) return -1;
    out->dx = mq[mtail].dx; out->dy = mq[mtail].dy;
    out->wheel = mq[mtail].wheel; out->buttons = mq[mtail].buttons;
    mtail = (mtail + 1) % MQ;
    return 0;
}

void mouse_present(const char *kind) { if (nmice < 4) mkinds[nmice++] = kind; }
unsigned mouse_count(void) { return nmice; }
const char *mouse_kind(unsigned i) { return i < nmice ? mkinds[i] : "?"; }
