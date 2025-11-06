#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector
{
    unsigned *data;
    size_t size;
    size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void data_vector_append_many(struct data_vector *vector,
                                    const unsigned *data, size_t count)
{
    if (vector->size + count > vector->capacity)
    {
        if (vector->capacity == 0)
            vector->capacity = 4;
        else
            vector->capacity *= 2;

        vector->data =
            realloc(vector->data, sizeof(vector->data[0]) * vector->capacity);
    }
    memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
    vector->size += count;
}

/** Append a single message to the vector. */
static void data_vector_append(struct data_vector *vector, unsigned data)
{
    data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void data_vector_pop_first_many(struct data_vector *vector,
                                       unsigned *data, size_t count)
{
    assert(count <= vector->size);
    memcpy(data, vector->data, sizeof(data[0]) * count);
    vector->size -= count;
    memmove(vector->data, &vector->data[count],
            vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned data_vector_pop_first(struct data_vector *vector)
{
    unsigned data = 0;
    data_vector_pop_first_many(vector, &data, 1);
    return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry
{
    struct rlist base;
    struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue
{
    struct rlist coros;
};

#if 1

/** Suspend the current coroutine until it is woken up. */
static void wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
    struct wakeup_entry *entry = malloc(sizeof(struct wakeup_entry));
    entry->coro = coro_this();
    rlist_add_tail_entry(&queue->coros, entry, base);
    coro_suspend();
    rlist_del_entry(entry, base);
    free(entry);
}

/** Wakeup the first coroutine in the queue. */
static void wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
    if (rlist_empty(&queue->coros))
        return;
    struct wakeup_entry *entry =
        rlist_first_entry(&queue->coros, struct wakeup_entry, base);
    coro_wakeup(entry->coro);
    rlist_del_entry(entry, base);
}

#endif

struct coro_bus_channel
{
    /** Channel max capacity. */
    size_t size_limit;
    /** Coroutines waiting until the channel is not full. */
    struct wakeup_queue send_queue;
    /** Coroutines waiting until the channel is not empty. */
    struct wakeup_queue recv_queue;
    /** Message queue. */
    struct data_vector data;
};

struct coro_bus
{
    struct coro_bus_channel **channels; //< Array of channels.
    int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code coro_bus_errno(void) { return global_error; }

void coro_bus_errno_set(enum coro_bus_error_code err) { global_error = err; }

struct coro_bus *coro_bus_new(void)
{
    struct coro_bus *ret_cb = calloc(1, sizeof(struct coro_bus));
    ret_cb->channels = NULL;
    ret_cb->channel_count = 0;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return ret_cb;
}

void coro_bus_delete(struct coro_bus *bus)
{
    for (int i = 0; i < bus->channel_count; i++)
    {
        if (bus->channels[i] != NULL)
        {
            coro_bus_channel_close(bus, i);
        }
    }
    free(bus->channels);
    free(bus);
}

static int find_empty_channel(struct coro_bus *bus)
{
    for (int i = 0; i < bus->channel_count; i++)
    {
        if (bus->channels[i] == NULL)
        {
            return i;
        }
    }
    return -1;
}

int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
    int empty_channel = find_empty_channel(bus);
    int ch_no = 0;

    if (empty_channel != -1)
    {
        ch_no = empty_channel;
    }
    else
    {
        bus->channel_count += 1;
        ch_no = bus->channel_count - 1;
        void* tmp = realloc(bus->channels, sizeof(bus->channels[0]) * (bus->channel_count));
        if (tmp == NULL)
        {
            free(bus->channels);
            return -1;
        }
        bus->channels = tmp;
    }

    bus->channels[ch_no] = calloc(1, sizeof(struct coro_bus_channel));
    struct coro_bus_channel *cur_ch = bus->channels[ch_no];
    cur_ch->size_limit = size_limit;
    cur_ch->data.data = calloc(1, sizeof(cur_ch->data.data[0]) * size_limit);
    cur_ch->data.size = 0;
    cur_ch->data.capacity = size_limit;

    rlist_create(&cur_ch->send_queue.coros);
    rlist_create(&cur_ch->recv_queue.coros);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return ch_no;
}

void coro_bus_channel_close(struct coro_bus *bus, int channel)
{
    if (channel < 0 || channel >= bus->channel_count)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return;
    }

    if (!bus->channels[channel])
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return;
    }

    struct coro_bus_channel *ch = bus->channels[channel];
    bus->channels[channel] = NULL;

    while (!rlist_empty(&ch->recv_queue.coros))
    {
        wakeup_queue_wakeup_first(&ch->recv_queue);
        coro_yield();
    }

    while (!rlist_empty(&ch->send_queue.coros))
    {
        wakeup_queue_wakeup_first(&ch->send_queue);
        coro_yield();
    }

    if (ch->data.data)
    {
        free(ch->data.data);
    }

    free(ch);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
    int res = coro_bus_try_send(bus, channel, data);
    while (res == -1)
    {
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK &&
            coro_bus_errno() != CORO_BUS_ERR_NO_CHANNEL &&
            coro_bus_errno() != CORO_BUS_ERR_NOT_IMPLEMENTED)
        {
            assert(0);
        }
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;
        if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
            coro_yield();
            res = coro_bus_try_send(bus, channel, data);
        }
    }
    if (res == 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return 0;
    }
    assert(0);
}

static int channel_full(struct coro_bus *bus, int channel)
{
    return bus->channels[channel]->data.size >=
           bus->channels[channel]->size_limit;
}

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
    if (channel < 0 || channel >= bus->channel_count ||
        bus->channels[channel] == NULL)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (channel_full(bus, channel))
    {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    data_vector_append(&bus->channels[channel]->data, data);
    wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
    //coro_yield();
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    int res = coro_bus_try_recv(bus, channel, data);
    while (res == -1)
    {
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK &&
            coro_bus_errno() != CORO_BUS_ERR_NO_CHANNEL &&
            coro_bus_errno() != CORO_BUS_ERR_NOT_IMPLEMENTED)
        {
            assert(0);
        }
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;
        if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
            coro_yield();
            res = coro_bus_try_recv(bus, channel, data);
        }
    }
    if (res == 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return 0;
    }
    assert(0);
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    if (channel < 0 || channel >= bus->channel_count ||
        bus->channels[channel] == NULL)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (bus->channels[channel]->data.size == 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    *data = data_vector_pop_first(&bus->channels[channel]->data);
    wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

#if NEED_BROADCAST

#define ALL_CHANNELS_NOT_FULL 0

static int check_all_channels(struct coro_bus *bus)
{
    int opened_channels = 0;
    for (int i = 0; i < bus->channel_count; i++)
    {
        if (bus->channels[i] == NULL)
        {
            continue;
        }
        if (channel_full(bus, i))
        {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }
        opened_channels++;
    }
    if (opened_channels == 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    return ALL_CHANNELS_NOT_FULL;
}

int coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
    int res = coro_bus_try_broadcast(bus, data);
    while (res == -1)
    {
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK &&
            coro_bus_errno() != CORO_BUS_ERR_NO_CHANNEL &&
            coro_bus_errno() != CORO_BUS_ERR_NOT_IMPLEMENTED)
        {
            assert(0);
        }
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;
        if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            for (int i = 0; i < bus->channel_count; i++)
            {
                if (bus->channels[i] && channel_full(bus, i))
                {
                    wakeup_queue_suspend_this(&bus->channels[i]->send_queue);
                    coro_yield();
                }
            }
            res = coro_bus_try_broadcast(bus, data);
        }
    }
    if (res == 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return 0;
    }
    assert(0);
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
    if (check_all_channels(bus) == ALL_CHANNELS_NOT_FULL)
    {
        for (int i = 0; i < bus->channel_count; ++i)
        {
            if (bus->channels[i] != NULL)
            {
                coro_bus_send(bus, i, data);
            }
        }
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return 0;
    }
    else
    {
        return -1;
    }
}

#endif

#if NEED_BATCH

int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                    unsigned count)
{
    int ret = coro_bus_try_send_v(bus, channel, data, count);
    while (ret == -1)
    {
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK &&
            coro_bus_errno() != CORO_BUS_ERR_NO_CHANNEL)
        {
            assert(0);
        }
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
        if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
            coro_yield();
            ret = coro_bus_try_send_v(bus, channel, data, count);
        }
    }
    if (ret >= 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return ret;
    }
    assert(0);
}

int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                        unsigned count)
{
    if (channel < 0 || channel >= bus->channel_count ||
        bus->channels[channel] == NULL)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (channel_full(bus, channel))
    {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    unsigned space =
        bus->channels[channel]->data.capacity - bus->channels[channel]->data.size;
    unsigned to_send = count < space ? count : space;

    data_vector_append_many(&bus->channels[channel]->data, data, to_send);
    wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return to_send;
}

int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                    unsigned capacity)
{
    int res = coro_bus_try_recv_v(bus, channel, data, capacity);
    while (res == -1)
    {
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK &&
            coro_bus_errno() != CORO_BUS_ERR_NO_CHANNEL)
        {
            assert(0);
        }
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;
        if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
        {
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
            coro_yield();
            res = coro_bus_try_recv_v(bus, channel, data, capacity);
        }
    }
    if (res >= 0)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NONE);
        return res;
    }
    assert(0);
}

int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                        unsigned capacity)
{
    if (channel < 0 || channel >= bus->channel_count ||
        bus->channels[channel] == NULL)
    {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    if (bus->channels[channel]->data.size == 0 && capacity != 0 && data != NULL)
    {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    int to_recv = capacity < bus->channels[channel]->data.size
                      ? capacity
                      : bus->channels[channel]->data.size;
    data_vector_pop_first_many(&bus->channels[channel]->data, data, to_recv);
    wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return to_recv;
}

#endif
