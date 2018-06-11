/*
** Copyright (C) 2015-2018 University of Oxford
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

#include "util.h"
#include "msprime.h"
#include "object_heap.h"

typedef struct {
    const char *ancestral_state;
    const char *derived_state;
} mutation_type_t;


static const mutation_type_t binary_mutation_types[] = {
    {"0", "1"}
};

static const mutation_type_t acgt_mutation_types[] = {
    {"A", "C"},
    {"A", "G"},
    {"A", "T"},
    {"C", "A"},
    {"C", "G"},
    {"C", "T"},
    {"G", "A"},
    {"G", "C"},
    {"G", "T"},
    {"T", "A"},
    {"T", "C"},
    {"T", "G"},
};

static int
cmp_site(const void *a, const void *b) {
    const site_t *ia = (const site_t *) a;
    const site_t *ib = (const site_t *) b;
    return (ia->position > ib->position) - (ia->position < ib->position);
}

static void
mutgen_check_state(mutgen_t *MSP_UNUSED(self))
{
    /* TODO some checks! */
}

void
mutgen_print_state(mutgen_t *self, FILE *out)
{
    fprintf(out, "Mutgen state\n");
    fprintf(out, "\tmutation_rate = %f\n", (double) self->mutation_rate);
    block_allocator_print_state(&self->allocator, out);
    printf("TODO write out AVL tree\n");
    mutgen_check_state(self);
}

int WARN_UNUSED
mutgen_alloc(mutgen_t *self, double mutation_rate, gsl_rng *rng, int alphabet,
        size_t block_size)
{
    int ret = 0;

    assert(rng != NULL);
    memset(self, 0, sizeof(mutgen_t));
    if (! (alphabet == MSP_ALPHABET_BINARY || alphabet == MSP_ALPHABET_NUCLEOTIDE)) {
        ret = MSP_ERR_BAD_PARAM_VALUE;
        goto out;
    }
    self->alphabet = alphabet;
    self->mutation_rate = mutation_rate;
    self->rng = rng;

    avl_init_tree(&self->sites, cmp_site, NULL);
    if (block_size == 0) {
        block_size = 8192;
    }
    /* In pracitise this is the minimum we can support */
    block_size = MSP_MAX(block_size, 128);
    ret = block_allocator_alloc(&self->allocator, block_size);
    if (ret != 0) {
        goto out;
    }
out:
    return ret;
}

int
mutgen_free(mutgen_t *self)
{
    block_allocator_free(&self->allocator);
    return 0;
}

static int WARN_UNUSED
mutgen_add_mutation(mutgen_t *self, node_id_t node, double position,
        const char *ancestral_state, const char *derived_state)
{
    int ret = 0;
    site_t *site = block_allocator_get(&self->allocator, sizeof(*site));
    mutation_t *mutation = block_allocator_get(&self->allocator, sizeof(*mutation));
    avl_node_t* avl_node = block_allocator_get(&self->allocator, sizeof(*avl_node));

    if (site == NULL || mutation == NULL || avl_node == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    memset(site, 0, sizeof(*site));
    memset(mutation, 0, sizeof(*mutation));
    site->position = position;
    site->ancestral_state = ancestral_state;
    site->ancestral_state_length = 1;
    mutation->derived_state = derived_state;
    mutation->derived_state_length = 1;
    mutation->node = node;
    mutation->parent = MSP_NULL_MUTATION;
    site->mutations = mutation;
    site->mutations_length = 1;

    avl_init_node(avl_node, site);
    avl_node = avl_insert_node(&self->sites, avl_node);
    assert(avl_node != NULL);
out:
    return ret;
}

static int
mutgen_populate_tables(mutgen_t *self, site_table_t *sites, mutation_table_t *mutations)
{
    int ret = 0;
    size_t j;
    site_id_t site_id;
    avl_node_t *a;
    site_t *site;
    mutation_t *mutation;

    for (a = self->sites.head; a != NULL; a = a->next) {
        site = (site_t *) a->item;
        site_id = site_table_add_row(sites, site->position, site->ancestral_state,
                site->ancestral_state_length, site->metadata, site->metadata_length);
        if (site_id < 0) {
            ret = site_id;
            goto out;
        }
        for (j = 0; j < site->mutations_length; j++) {
            mutation = site->mutations + j;
            ret = mutation_table_add_row(mutations, site_id,
                    mutation->node, mutation->parent,
                    mutation->derived_state, mutation->derived_state_length,
                    mutation->metadata, mutation->metadata_length);
            if (ret < 0) {
                goto out;
            }
        }
    }
    ret = 0;
out:
    return ret;
}

int WARN_UNUSED
mutgen_generate(mutgen_t *self, table_collection_t *tables, int flags)
{
    int ret;
    node_table_t *nodes = tables->nodes;
    edge_table_t *edges = tables->edges;
    size_t j, l, branch_mutations;
    double left, right, branch_length, distance, mu, position;
    node_id_t parent, child;
    const mutation_type_t *mutation_types;
    unsigned long num_mutation_types;
    const char *ancestral_state, *derived_state;
    unsigned long type;
    avl_node_t *avl_node;

    assert(flags == 0);

    avl_clear_tree(&self->sites);
    block_allocator_reset(&self->allocator);

    ret = site_table_clear(tables->sites);
    if (ret != 0) {
        goto out;
    }
    ret = mutation_table_clear(tables->mutations);
    if (ret != 0) {
        goto out;
    }

    if (self->alphabet == 0) {
        mutation_types = binary_mutation_types;
        num_mutation_types = 1;
    } else {
        mutation_types = acgt_mutation_types;
        num_mutation_types = 12;
    }

    for (j = 0; j < edges->num_rows; j++) {
        left = edges->left[j];
        right = edges->right[j];
        distance = right - left;
        parent = edges->parent[j];
        child = edges->child[j];
        assert(child >= 0 && child < (node_id_t) nodes->num_rows);
        branch_length = nodes->time[parent] - nodes->time[child];
        mu = branch_length * distance * self->mutation_rate;
        branch_mutations = gsl_ran_poisson(self->rng, mu);
        for (l = 0; l < branch_mutations; l++) {
            /* Rejection sample positions until we get one we haven't seen before. */
            /* TODO add a maximum number of rejections here */
            do {
                position = gsl_ran_flat(self->rng, left, right);
                avl_node = avl_search(&self->sites, &position);
            } while (avl_node != NULL);
            assert(left <= position && position < right);
            type = gsl_rng_uniform_int(self->rng, num_mutation_types);
            ancestral_state = mutation_types[type].ancestral_state;
            derived_state = mutation_types[type].derived_state;
            ret = mutgen_add_mutation(self, child, position, ancestral_state,
                    derived_state);
            if (ret != 0) {
                goto out;
            }
        }
    }
    ret = mutgen_populate_tables(self, tables->sites, tables->mutations);
    if (ret != 0) {
        goto out;
    }
out:
    return ret;
}
