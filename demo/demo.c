#include <stdio.h>
#include <string.h>
#include "osmem.h"

int main(void)
{
	char *a, *b, *c, *big;
	int *arr;

	a = os_malloc(32);
	strcpy(a, "Hello from my allocator!");
	printf("a   = %p  \"%s\"\n", (void *)a, a);

	arr = os_calloc(10, sizeof(int));
	for (int i = 0; i < 10; i++)
		arr[i] = i * i;
	printf("arr = %p  arr[9] = %d\n", (void *)arr, arr[9]);

	b = os_malloc(100);
	printf("b   = %p\n", (void *)b);
	os_free(b);
	c = os_malloc(80);
	printf("c   = %p\n", (void *)c);

	a = os_realloc(a, 4000);
	printf("a   = %p  \"%s\"\n", (void *)a, a);

	big = os_malloc(1024 * 1024);
	printf("big = %p\n", (void *)big);

	os_free(a);
	os_free(arr);
	os_free(c);
	os_free(big);
	return 0;
}