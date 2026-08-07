#pragma once

#include <linux/kernel.h>

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *entry,
                              struct list_head *previous,
                              struct list_head *next) {
    next->prev = entry;
    entry->next = next;
    entry->prev = previous;
    previous->next = entry;
}

static inline void list_add(struct list_head *entry, struct list_head *head) {
    __list_add(entry, head, head->next);
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head) {
    __list_add(entry, head->prev, head);
}

static inline void __list_del(struct list_head *previous, struct list_head *next) {
    next->prev = previous;
    previous->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = 0;
    entry->prev = 0;
}

static inline void list_del_init(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

#define list_entry(pointer, type, member) container_of(pointer, type, member)
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)

#define list_for_each(position, head) \
    for ((position) = (head)->next; (position) != (head); (position) = (position)->next)

#define list_for_each_safe(position, next_position, head)                    \
    for ((position) = (head)->next, (next_position) = (position)->next;      \
         (position) != (head);                                               \
         (position) = (next_position), (next_position) = (position)->next)

#define list_for_each_entry(position, head, member)                          \
    for ((position) = list_entry((head)->next, __typeof__(*(position)), member); \
         &(position)->member != (head);                                      \
         (position) = list_entry((position)->member.next, __typeof__(*(position)), member))
