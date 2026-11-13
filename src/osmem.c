// SPDX-License-Identifier: BSD-3-Clause

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT	8
#define ALIGN(x)	(((x) + (ALIGNMENT - 1)) & ~((size_t)ALIGNMENT - 1))
#define META_SIZE	ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD	(128 * 1024)
#define HEAP_PREALLOC	(128 * 1024)
/* Найбільший запит, для якого ALIGN(size) + META_SIZE не переповнює size_t. */
#define MAX_REQUEST	(SIZE_MAX - META_SIZE - ALIGNMENT)

#define PAYLOAD(block)	((void *)((char *)(block) + META_SIZE))
#define META(ptr)	((struct block_meta *)((char *)(ptr) - META_SIZE))

/*
 * Двозв'язний список блоків купи в порядку адрес. Блоки, виділені через
 * mmap(), до списку не входять: вони не суміжні з купою, а звільняються
 * цілком через munmap(), тому зв'язувати їх немає потреби.
 */
static struct block_meta *heap_head;
static struct block_meta *heap_tail;
static int heap_preallocated;

static void *heap_grow(size_t increment)
{
	void *old_brk = sbrk((intptr_t)increment);

	DIE(old_brk == (void *)-1, "sbrk");
	return old_brk;
}

static void heap_append(struct block_meta *block)
{
	block->prev = heap_tail;
	block->next = NULL;

	if (heap_tail)
		heap_tail->next = block;
	else
		heap_head = block;

	heap_tail = block;
}

/* Перший запит до купи резервує одразу 128 КіБ, щоб зменшити кількість brk(). */
static void heap_preallocate(void)
{
	struct block_meta *block = heap_grow(HEAP_PREALLOC);

	block->size = HEAP_PREALLOC - META_SIZE;
	block->status = STATUS_FREE;
	heap_append(block);
	heap_preallocated = 1;
}

/*
 * Відрізає від блока рівно size байтів корисного навантаження, а залишок
 * оформлює окремим вільним блоком. Розміри кратні 8, тому «хоча б 1 байт»
 * на практиці означає хоча б ALIGNMENT байтів.
 */
static void block_split(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + ALIGNMENT)
		return;

	rest = (struct block_meta *)((char *)PAYLOAD(block) + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;

	if (block->next)
		block->next->prev = rest;
	else
		heap_tail = rest;

	block->next = rest;
	block->size = size;
}

/* Приєднує до блока наступний за ним блок, поглинаючи і його заголовок. */
static void block_absorb_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;

	if (next->next)
		next->next->prev = block;
	else
		heap_tail = block;
}

static void heap_coalesce(void)
{
	struct block_meta *block = heap_head;

	while (block && block->next) {
		if (block->status == STATUS_FREE && block->next->status == STATUS_FREE)
			block_absorb_next(block);
		else
			block = block->next;
	}
}

/* Best fit: найменший вільний блок, у який вміщується запит. */
static struct block_meta *heap_find_best(size_t size)
{
	struct block_meta *best = NULL;
	struct block_meta *block;

	for (block = heap_head; block; block = block->next) {
		if (block->status != STATUS_FREE || block->size < size)
			continue;

		if (!best || block->size < best->size)
			best = block;
	}

	return best;
}

static struct block_meta *heap_alloc(size_t size)
{
	struct block_meta *block;

	if (!heap_preallocated)
		heap_preallocate();

	heap_coalesce();

	block = heap_find_best(size);
	if (block) {
		block_split(block, size);
		block->status = STATUS_ALLOC;
		return block;
	}

	/* Вільний останній блок розширюємо лише на відсутню частину. */
	if (heap_tail && heap_tail->status == STATUS_FREE) {
		block = heap_tail;
		heap_grow(size - block->size);
		block->size = size;
		block->status = STATUS_ALLOC;
		return block;
	}

	block = heap_grow(META_SIZE + size);
	block->size = size;
	block->status = STATUS_ALLOC;
	heap_append(block);

	return block;
}

static struct block_meta *map_alloc(size_t size)
{
	struct block_meta *block;

	block = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANON, -1, 0);
	DIE(block == MAP_FAILED, "mmap");

	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;

	return block;
}

/* Поріг враховує і заголовок: malloc порівнює зі 128 КіБ, calloc — зі сторінкою. */
static struct block_meta *block_alloc(size_t size, size_t threshold)
{
	size_t aligned = ALIGN(size);

	if (META_SIZE + aligned >= threshold)
		return map_alloc(aligned);

	return heap_alloc(aligned);
}

void *os_malloc(size_t size)
{
	if (size == 0 || size > MAX_REQUEST)
		return NULL;

	return PAYLOAD(block_alloc(size, MMAP_THRESHOLD));
}

/*
 * Вільні блоки не об'єднуємо тут: це робиться перед пошуком у heap_alloc()
 * і поблоково в os_realloc(), як вимагає завдання.
 */
void os_free(void *ptr)
{
	struct block_meta *block;
	int ret;

	if (!ptr)
		return;

	block = META(ptr);

	if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
	} else if (block->status == STATUS_MAPPED) {
		ret = munmap(block, META_SIZE + block->size);
		DIE(ret == -1, "munmap");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	struct block_meta *block;
	size_t bytes;

	if (nmemb == 0 || size == 0 || nmemb > MAX_REQUEST / size)
		return NULL;

	bytes = nmemb * size;
	block = block_alloc(bytes, (size_t)getpagesize());

	/* Сторінки від mmap() ядро вже заповнило нулями. */
	if (block->status != STATUS_MAPPED)
		memset(PAYLOAD(block), 0, bytes);

	return PAYLOAD(block);
}

/* Намагається розширити блок купи на місці; повертає 1, якщо вдалося. */
static int heap_expand(struct block_meta *block, size_t size)
{
	if (block == heap_tail) {
		heap_grow(size - block->size);
		block->size = size;
		return 1;
	}

	/* Об'єднані блоки лишаються об'єднаними, навіть якщо місця не вистачило. */
	while (block->next && block->next->status == STATUS_FREE) {
		block_absorb_next(block);

		if (block->size >= size) {
			block_split(block, size);
			return 1;
		}
	}

	return 0;
}

/* Виділяє новий блок, копіює дані й звільняє старий. */
static void *block_move(struct block_meta *block, size_t size)
{
	struct block_meta *moved = block_alloc(size, MMAP_THRESHOLD);
	size_t bytes = block->size < size ? block->size : size;

	memcpy(PAYLOAD(moved), PAYLOAD(block), bytes);
	os_free(PAYLOAD(block));

	return PAYLOAD(moved);
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;
	size_t aligned;

	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	if (size > MAX_REQUEST)
		return NULL;

	block = META(ptr);
	aligned = ALIGN(size);

	if (block->status == STATUS_FREE)
		return NULL;

	if (block->status == STATUS_MAPPED || META_SIZE + aligned >= MMAP_THRESHOLD)
		return block_move(block, aligned);

	if (block->size >= aligned) {
		block_split(block, aligned);
		return ptr;
	}

	if (heap_expand(block, aligned))
		return ptr;

	return block_move(block, aligned);
}
