/*-------------------------------------------------------------------------
 *
 * vector.c
 *
 * Growable array implementation.
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/tools/pgguclifetimes/vector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "vector.h"

bool
vector_init(struct vector *vec, size_t capacity)
{
	Assert(vec != NULL);
	Assert(capacity > 0);

	vec->len = 0;
	vec->data = malloc(capacity * sizeof(*vec->data));

	if (vec->data == NULL) {
		vec->cap = 0;
		return false;
	}

	vec->cap = capacity;

	return true;
}

bool
vector_append(struct vector *vec, char *item)
{
	if (vec->len == vec->cap) {
		void *tmp;

		vec->cap *= 2;
		tmp = realloc(vec->data, vec->cap * sizeof(*vec->data));
		if (tmp == NULL)
			return false;

		vec->data = tmp;
	}

	vec->data[vec->len++] = item;

	return true;
}

void
vector_free(struct vector *vec)
{
	for (size_t i = 0; i < vec->len; i++)
		free(vec->data[i]);
	free(vec->data);
}
