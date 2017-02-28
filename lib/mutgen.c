/*
** Copyright (C) 2015 Jerome Kelleher <jerome.kelleher@well.ox.ac.uk>
**
** This file is part of msprime.
**
** msprime is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** msprime is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with msprime.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <gsl/gsl_randist.h>

#include "err.h"
#include "msprime.h"
#include "object_heap.h"


static int
cmp_mutation(const void *a, const void *b) {
    const mutation_t *ia = (const mutation_t *) a;
    const mutation_t *ib = (const mutation_t *) b;
    return (ia->position > ib->position) - (ia->position < ib->position);
}

static void
mutgen_check_state(mutgen_t *self)
{
    /* TODO some checks! */
}

void
mutgen_print_state(mutgen_t *self, FILE *out)
{
    size_t j, k;

    fprintf(out, "Mutgen state\n");
    fprintf(out, "\tmutation_rate = %f\n", (double) self->mutation_rate);
    fprintf(out, "\tmutation_block_size = %d\n", (int) self->mutation_block_size);
    fprintf(out, "\tmax_num_mutations  = %d\n", (int) self->max_num_mutations);
    fprintf(out, "\tnode_heap  = \n");
    object_heap_print_state(&self->node_heap, out);
    fprintf(out, "mutations\t%d\n", (int) self->num_mutations);
    for (j = 0; j < self->num_mutations; j++) {
        fprintf(out, "\t%f\t%d\t", self->mutations[j].position,
                self->mutations[j].type);
        for (k = 0; k < self->mutations[j].num_nodes; k++) {
            fprintf(out, "%d,", (int) self->mutations[j].nodes[k]);
        }
        fprintf(out, "\n");
    }
    mutgen_check_state(self);
}


int WARN_UNUSED
mutgen_alloc(mutgen_t *self, double mutation_rate, gsl_rng *rng)
{
    int ret = MSP_ERR_NO_MEMORY;

    assert(rng != NULL);
    memset(self, 0, sizeof(mutgen_t));
    self->mutation_rate = mutation_rate;
    self->rng = rng;
    self->num_mutations = 0;
    self->mutation_block_size = 1024 * 1024;
    /* Avoid potential portability issues with realloc(NULL, newsize)
     * by mallocing enough space for 1 mutation initially. This gives the user
     * control over the overall malloc behavior.
     */
    self->max_num_mutations = 1;
    self->mutations = malloc(self->max_num_mutations * sizeof(mutation_t));
    if (self->mutations == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    ret = object_heap_init(&self->node_heap, sizeof(node_id_t),
            self->mutation_block_size, NULL);
    if (ret != 0) {
        goto out;
    }
out:
    return ret;
}

int
mutgen_free(mutgen_t *self)
{
    if (self->mutations != NULL) {
        free(self->mutations);
    }
    object_heap_free(&self->node_heap);
    return 0;
}

int WARN_UNUSED
mutgen_set_mutation_block_size(mutgen_t *self, size_t mutation_block_size)
{
    int ret = 0;
    if (mutation_block_size == 0) {
        ret = MSP_ERR_BAD_PARAM_VALUE;
        goto out;
    }
    self->mutation_block_size = mutation_block_size;
out:
    return ret;
}

static int WARN_UNUSED
mutgen_add_mutation(mutgen_t *self, node_id_t node, double position,
        mutation_type_id_t type)
{
    int ret = 0;
    mutation_t *tmp_buffer;
    node_id_t *p;

    assert(self->num_mutations <= self->max_num_mutations);

    if (self->num_mutations == self->max_num_mutations) {
        self->max_num_mutations += self->mutation_block_size;
        tmp_buffer = realloc(self->mutations, self->max_num_mutations * sizeof(mutation_t));
        if (tmp_buffer == NULL) {
            ret = MSP_ERR_NO_MEMORY;
            goto out;
        }
        self->mutations = tmp_buffer;
    }
    if (object_heap_empty(&self->node_heap)) {
        ret = object_heap_expand(&self->node_heap);
        if ( ret != 0) {
            goto out;
        }
    }
    p = (node_id_t *) object_heap_alloc_object(&self->node_heap);
    self->mutations[self->num_mutations].nodes = p;
    self->mutations[self->num_mutations].nodes[0] = node;
    self->mutations[self->num_mutations].num_nodes = 1;
    self->mutations[self->num_mutations].position = position;
    self->mutations[self->num_mutations].type = type;
    self->num_mutations++;
out:
    return ret;
}

/* temporary interface while we're working out the details of passing around
 * data via tables. */
int WARN_UNUSED
mutgen_generate_tables_tmp(mutgen_t *self, node_table_t *nodes,
        edgeset_table_t *edgesets)
{
    int ret;
    size_t j, l, offset, branch_mutations;
    double left, right, branch_length, distance, mu, position;
    node_id_t parent, child;

    /* First free up any memory used in previous calls */
    for (j = 0; j < self->num_mutations; j++) {
        object_heap_free_object(&self->node_heap, self->mutations[j].nodes);
    }
    self->num_mutations = 0;

    offset = 0;
    for (j = 0; j < edgesets->num_rows; j++) {
        left = edgesets->left[j];
        right = edgesets->right[j];
        distance = right - left;
        parent = edgesets->parent[j];
        while (offset < edgesets->children_length
                && edgesets->children[offset] != MSP_NULL_NODE) {
            child = edgesets->children[offset];
            offset++;
            branch_length = nodes->time[parent] - nodes->time[child];
            mu = branch_length * distance * self->mutation_rate;
            branch_mutations = gsl_ran_poisson(self->rng, mu);
            for (l = 0; l < branch_mutations; l++) {
                position = gsl_ran_flat(self->rng, left, right);
                assert(left <= position && position < right);
                ret = mutgen_add_mutation(self, child, position, 0);
                if (ret != 0) {
                    goto out;
                }
            }
        }
        offset++;
    }
    qsort(self->mutations, self->num_mutations, sizeof(mutation_t), cmp_mutation);
    ret = 0;
out:
    return ret;
}

int
mutgen_populate_tables(mutgen_t *self, mutation_type_table_t *mutation_types,
        mutation_table_t *mutations)
{
    int ret = 0;
    mutation_t *mut;
    size_t j;

    ret = mutation_type_table_reset(mutation_types);
    if (ret != 0) {
        goto out;
    }
    ret = mutation_table_reset(mutations);
    if (ret != 0) {
        goto out;
    }
    ret = mutation_type_table_add_row(mutation_types, "0", "1");
    if (ret != 0) {
        goto out;
    }
    for (j = 0; j < self->num_mutations; j++) {
        mut = &self->mutations[j];
        ret = mutation_table_add_row(mutations, mut->position,
                mut->num_nodes, mut->nodes, 0);
        if (ret != 0) {
            goto out;
        }
    }
out:
    return ret;
}

size_t
mutgen_get_num_mutations(mutgen_t *self)
{
    return self->num_mutations;
}

size_t
mutgen_get_total_nodes(mutgen_t *self)
{
    return self->num_mutations;
}

int  WARN_UNUSED
mutgen_get_mutations(mutgen_t *self, mutation_t **mutations)
{
    *mutations = self->mutations;
    return 0;
}
