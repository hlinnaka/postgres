/*-------------------------------------------------------------------------
 *
 * vector.h
 *
 * Growable array implementation.
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/tools/pgguclifetimes/vector.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGGUCLIFETIMES_VECTOR_H
#define PGGUCLIFETIMES_VECTOR_H

#include "c.h"

struct vector {
	char **data;
	size_t cap;
	size_t len;
};

extern bool
vector_init(struct vector *vec, size_t capacity);

extern bool
vector_append(struct vector *vec, char *item);

void
vector_free(struct vector *vec);

#endif /* PGGUCLIFETIMES_VECTOR_H */
