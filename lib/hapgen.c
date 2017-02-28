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

#include "err.h"
#include "object_heap.h"
#include "msprime.h"

#define HG_WORD_SIZE 64

/* Ensure the tree is in a consistent state */
static void
hapgen_check_state(hapgen_t *self)
{
    /* TODO some checks! */
}

void
hapgen_print_state(hapgen_t *self, FILE *out)
{
    size_t j, k;

    fprintf(out, "Hapgen state\n");
    fprintf(out, "num_mutations = %d\n", (int) self->num_mutations);
    fprintf(out, "words_per_row = %d\n", (int) self->words_per_row);
    fprintf(out, "haplotype matrix\n");
    for (j = 0; j < self->sample_size; j++) {
        for (k = 0; k < self->words_per_row; k++) {
            fprintf(out, "%llu ", (unsigned long long)
                    self->haplotype_matrix[j * self->words_per_row + k]);
        }
        fprintf(out, "\n");
    }
    hapgen_check_state(self);
}

static inline int
hapgen_set_bit(hapgen_t *self, size_t row, size_t column)
{
    int ret = 0;
    /* get the word that column falls in */
    size_t word = column / HG_WORD_SIZE;
    size_t bit = column % HG_WORD_SIZE;
    size_t index = row * self->words_per_row + word;

    assert(word < self->words_per_row);
    if ((self->haplotype_matrix[index] & (1ULL << bit)) != 0) {
        ret = MSP_ERR_INCONSISTENT_MUTATIONS;
        goto out;
    }
    self->haplotype_matrix[index] |= 1ULL << bit;
out:
    return ret;
}

static int
hapgen_apply_tree_mutation(hapgen_t *self, mutation_t *mut)
{
    int ret = 0;
    leaf_list_node_t *w, *tail;
    bool not_done;
    uint32_t j;

    for (j = 0; j < mut->num_nodes; j++) {
        ret = sparse_tree_get_leaf_list(&self->tree, mut->nodes[j], &w, &tail);
        if (ret != 0) {
            goto out;
        }
        if (w != NULL) {
            not_done = true;
            while (not_done) {
                assert(w != NULL);
                ret = hapgen_set_bit(self, (size_t) w->node, mut->index);
                if (ret != 0) {
                    goto out;
                }
                not_done = w != tail;
                w = w->next;
            }
        }
    }
out:
    return ret;
}

static int
hapgen_generate_all_haplotypes(hapgen_t *self)
{
    int ret = 0;
    size_t j;
    sparse_tree_t *t = &self->tree;

    for (ret = sparse_tree_first(t); ret == 1; ret = sparse_tree_next(t)) {
        for (j = 0; j < t->num_mutations; j++) {
            ret = hapgen_apply_tree_mutation(self, &t->mutations[j]);
            if (ret != 0) {
                goto out;
            }
        }
    }
out:
    return ret;
}

int
hapgen_alloc(hapgen_t *self, tree_sequence_t *tree_sequence)
{
    int ret = MSP_ERR_NO_MEMORY;

    assert(tree_sequence != NULL);
    memset(self, 0, sizeof(hapgen_t));
    self->sample_size = tree_sequence_get_sample_size(tree_sequence);
    self->sequence_length = tree_sequence_get_sequence_length(tree_sequence);
    self->num_mutations = tree_sequence_get_num_mutations(tree_sequence);
    self->tree_sequence = tree_sequence;

    if (self->num_mutations > 0 && tree_sequence->mutation_types.num_records != 1) {
        ret = MSP_ERR_NONBINARY_MUTATIONS_UNSUPPORTED;
        goto out;
    }

    ret = sparse_tree_alloc(&self->tree, tree_sequence, MSP_LEAF_LISTS);
    if (ret != 0) {
        goto out;
    }
    /* set up the haplotype binary matrix */
    /* The number of words per row is the number of mutations divided by 64 */
    self->words_per_row = (self->num_mutations / HG_WORD_SIZE) + 1;
    self->haplotype_matrix = calloc(self->words_per_row * self->sample_size,
            sizeof(uint64_t));
    self->haplotype = malloc(self->words_per_row * HG_WORD_SIZE + 1);
    if (self->haplotype_matrix == NULL || self->haplotype == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    ret = hapgen_generate_all_haplotypes(self);
    if (ret != 0) {
        goto out;
    }
    ret = 0;
out:
    return ret;
}

int
hapgen_free(hapgen_t *self)
{
    if (self->haplotype_matrix != NULL) {
        free(self->haplotype_matrix);
    }
    if (self->haplotype != NULL) {
        free(self->haplotype);
    }
    sparse_tree_free(&self->tree);
    return 0;
}

int
hapgen_get_haplotype(hapgen_t *self, node_id_t sample_id, char **haplotype)
{
    int ret = 0;
    size_t j, k, l, word_index;
    uint64_t word;

    if (sample_id >= (node_id_t) self->sample_size) {
        ret = MSP_ERR_OUT_OF_BOUNDS;
        goto out;
    }
    l = 0;
    for (j = 0; j < self->words_per_row; j++) {
        word_index = ((size_t) sample_id) * self->words_per_row + j;
        word = self->haplotype_matrix[word_index];
        for (k = 0; k < HG_WORD_SIZE; k++) {
            self->haplotype[l] = (word >> k) & 1ULL ? '1': '0';
            l++;
        }
    }
    self->haplotype[self->num_mutations] = '\0';
    *haplotype = self->haplotype;
out:
    return ret;
}
