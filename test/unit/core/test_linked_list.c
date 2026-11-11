/*
 * Source/linked_list.c -- intrusive doubly linked list, driven through its public API.
 * Real object code from libavos_cov.a; nothing is replaced.
 */
#include "avos_test.h"
#include "linked_list.h"
#include "astdlib.h"

typedef struct { LinkedListNode n; int v; } Item;

/* findNext/findPrev are exported by linked_list.c but not declared in its header. */
LinkedListNode *LinkedListNode_findPrev(LinkedListNode *obj, const Class *cls);
LinkedListNode *LinkedListNode_findNext(LinkedListNode *obj, const Class *cls);

static Class Item_class = { &LinkedListNode_class, "Item" };     /* Item extends LinkedListNode */

static Item *item(int v)
{
    Item *it = amalloc(sizeof *it);
    LinkedListNode_init(&it->n);
    it->v = v;
    return it;
}
static int val_at(LinkedList *l, int i) { return ((Item *)LinkedList_entryAt(l, i))->v; }

/* Every structural invariant: counted forward and backward, prev/next symmetric, ends terminate. */
static void check_list(LinkedList *l, const int *want, int n)
{
    CHECK_EQ_INT(LinkedList_count(l), n);
    CHECK(LinkedList_invariantOk(l));
    LinkedListNode *a = l->first, *prev = NULL;
    for (int i = 0; i < n; i++) {
        REQUIRE_NOT_NULL(a);
        CHECK_EQ_INT(((Item *)a)->v, want[i]);
        CHECK(a->prev == prev);
        prev = a;
        a = a->next;
    }
    CHECK_NULL(a);
    CHECK(l->last == prev);
    if (n == 0) { CHECK_NULL(l->first); CHECK_NULL(l->last); }
}

TEST(empty_list)
{
    LinkedList l;
    LinkedList_init(&l);
    check_list(&l, NULL, 0);
    CHECK_NULL(LinkedList_entryAt(&l, 0));
    CHECK_NULL(LinkedList_entryAt(&l, -1));
    Item *x = item(1);
    CHECK(!LinkedList_contains(&l, &x->n));
    CHECK(!LinkedList_contains(&l, NULL));
    CHECK_EQ_INT(LinkedList_indexOf(&l, &x->n), -1);
    afree(x);
}

TEST(append_prepend_and_insert_keep_order)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *a = item(1), *b = item(2), *c = item(3), *d = item(4), *e = item(5);

    LinkedList_append(&l, &c->n);                 /* c            */
    LinkedList_prepend(&l, &a->n);                /* a c          */
    LinkedList_insertBefore(&l, &b->n, &c->n);    /* a b c        */
    LinkedList_insertAfter(&l, &d->n, &c->n);     /* a b c d      */
    check_list(&l, (int[]){1, 2, 3, 4}, 4);

    LinkedList_insertBefore(&l, &e->n, &a->n);    /* before the head: e becomes first */
    check_list(&l, (int[]){5, 1, 2, 3, 4}, 5);

    const int order[] = {5, 1, 2, 3, 4};
    for (int i = 0; i < 5; i++) CHECK_EQ_INT(val_at(&l, i), order[i]);
    CHECK_NULL(LinkedList_entryAt(&l, 5));
    CHECK_EQ_INT(LinkedList_indexOf(&l, &d->n), 4);
    CHECK_EQ_INT(LinkedList_indexOf(&l, &e->n), 0);
    CHECK(LinkedList_contains(&l, &b->n));

    LinkedList_freeEntries(&l);
    check_list(&l, NULL, 0);
}

TEST(insert_into_the_ends_updates_first_and_last)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *a = item(1), *b = item(2), *c = item(3);
    LinkedList_append(&l, &a->n);
    LinkedList_insertAfter(&l, &b->n, &a->n);     /* after the tail: b becomes last */
    CHECK(l.last == &b->n);
    LinkedList_prepend(&l, &c->n);                /* prepend onto non-empty list     */
    CHECK(l.first == &c->n);
    check_list(&l, (int[]){3, 1, 2}, 3);
    LinkedList_freeEntries(&l);
}

TEST(remove_head_middle_tail_and_only_element)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *n[4];
    for (int i = 0; i < 4; i++) { n[i] = item(i); LinkedList_append(&l, &n[i]->n); }

    CHECK(LinkedList_remove(&l, &n[1]->n) == &n[1]->n);        /* middle */
    check_list(&l, (int[]){0, 2, 3}, 3);
    CHECK_NULL(n[1]->n.prev);                                    /* removed node is fully detached */
    CHECK_NULL(n[1]->n.next);
    CHECK(!LinkedList_contains(&l, &n[1]->n));

    LinkedList_remove(&l, &n[0]->n);                             /* head */
    check_list(&l, (int[]){2, 3}, 2);
    LinkedList_remove(&l, &n[3]->n);                             /* tail */
    check_list(&l, (int[]){2}, 1);
    LinkedList_remove(&l, &n[2]->n);                             /* only element */
    check_list(&l, NULL, 0);

    /* a removed node can be re-queued */
    LinkedList_append(&l, &n[1]->n);
    check_list(&l, (int[]){1}, 1);
    LinkedList_freeEntries(&l);
    afree(n[0]); afree(n[2]); afree(n[3]);
}

static void *dup_item(void *p)
{
    Item *src = p, *copy = amalloc(sizeof *copy);
    *copy = *src;
    return copy;
}
static void *is_even(void *p, void *extra)
{
    int *seen = extra;
    if (seen) (*seen)++;
    return (void *)(intptr_t)(((Item *)p)->v % 2 == 0);
}

TEST(duplicate_copies_every_node_in_order_and_leaves_source_alone)
{
    LinkedList l;
    LinkedList_init(&l);
    for (int i = 1; i <= 4; i++) LinkedList_append(&l, &item(i)->n);

    LinkedList *copy = LinkedList_duplicate(&l, dup_item);
    REQUIRE_NOT_NULL(copy);
    check_list(copy, (int[]){1, 2, 3, 4}, 4);
    check_list(&l, (int[]){1, 2, 3, 4}, 4);
    CHECK(copy->first != l.first);                               /* a real copy, not shared nodes */

    LinkedList_freeEntries(copy);
    afree(copy);
    LinkedList_freeEntries(&l);
}

TEST(duplicate_if_filters_and_passes_extra_data)
{
    LinkedList l;
    LinkedList_init(&l);
    for (int i = 1; i <= 6; i++) LinkedList_append(&l, &item(i)->n);

    int visited = 0;
    LinkedList *evens = LinkedList_duplicate_if(&l, dup_item, is_even, &visited);
    REQUIRE_NOT_NULL(evens);
    CHECK_EQ_INT(visited, 6);                                    /* predicate saw every node */
    check_list(evens, (int[]){2, 4, 6}, 3);

    LinkedList empty;
    LinkedList_init(&empty);
    LinkedList *none = LinkedList_duplicate(&empty, dup_item);   /* duplicating an empty list */
    REQUIRE_NOT_NULL(none);
    check_list(none, NULL, 0);

    LinkedList_freeEntries(evens); afree(evens);
    afree(none);
    LinkedList_freeEntries(&l);
}

TEST(find_next_and_prev_match_by_class_and_skip_the_rest)
{
    /* list:  P0  I1  P2  P3  I4  P5      (P = plain LinkedListNode, I = Item subclass) */
    LinkedList l;
    LinkedList_init(&l);
    Item *n[6];
    for (int i = 0; i < 6; i++) {
        n[i] = item(i);
        n[i]->n.s_object.cls = (i == 1 || i == 4) ? &Item_class : &LinkedListNode_class;
        LinkedList_append(&l, &n[i]->n);
    }
    /* asking for the base class matches everything, so the very next/previous node comes back */
    CHECK(LinkedListNode_findNext(&n[0]->n, &LinkedListNode_class) == &n[1]->n);
    CHECK(LinkedListNode_findPrev(&n[5]->n, &LinkedListNode_class) == &n[4]->n);
    /* asking for the subclass skips the plain nodes in between */
    CHECK(LinkedListNode_findNext(&n[1]->n, &Item_class) == &n[4]->n);
    CHECK(LinkedListNode_findPrev(&n[4]->n, &Item_class) == &n[1]->n);
    CHECK(LinkedListNode_findNext(&n[0]->n, &Item_class) == &n[1]->n);
    CHECK(LinkedListNode_findPrev(&n[5]->n, &Item_class) == &n[4]->n);
    /* the search starts AFTER/BEFORE the node itself, never returns the node */
    CHECK_NULL(LinkedListNode_findNext(&n[4]->n, &Item_class));
    CHECK_NULL(LinkedListNode_findPrev(&n[1]->n, &Item_class));
    /* nothing matches an unrelated class, and the ends of the list return NULL */
    static Class other = { &Object_class, "Other" };
    CHECK_NULL(LinkedListNode_findNext(&n[0]->n, &other));
    CHECK_NULL(LinkedListNode_findPrev(&n[5]->n, &other));
    CHECK_NULL(LinkedListNode_findNext(&n[5]->n, &LinkedListNode_class));
    CHECK_NULL(LinkedListNode_findPrev(&n[0]->n, &LinkedListNode_class));
    LinkedList_freeEntries(&l);
}

TEST(contract_violations_assert)
{
    LinkedList l, other;
    LinkedList_init(&l);
    LinkedList_init(&other);
    Item *a = item(1), *b = item(2);
    LinkedList_append(&l, &a->n);

    CHECK_ABORTS(LinkedList_append(&l, &a->n));                  /* already queued          */
    CHECK_ABORTS(LinkedList_prepend(&l, &a->n));
    CHECK_ABORTS(LinkedList_insertBefore(&l, &a->n, &a->n));
    CHECK_ABORTS(LinkedList_insertAfter(&l, &a->n, &a->n));
    CHECK_ABORTS(LinkedList_remove(&l, &b->n));                  /* never queued            */
    CHECK_SURVIVES(LinkedList_remove(&l, &a->n));                /* the legal call survives */
    afree(a);
    afree(b);
}

TEST(dump_helpers_do_not_crash)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *x = item(7);
    LinkedList_append(&l, &x->n);
    CHECK_SURVIVES(LinkedList_dump(&l));
    CHECK_SURVIVES(LinkedList_dumpTestNodes(&l));
    CHECK_SURVIVES(LinkedList_test());                           /* the module's own built-in scenario */
    afree(x);
}

TEST(prepend_onto_empty_list_sets_both_ends)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *a = item(1);
    LinkedList_prepend(&l, &a->n);
    CHECK(l.first == &a->n);
    CHECK(l.last == &a->n);
    check_list(&l, (int[]){1}, 1);
    LinkedList_freeEntries(&l);
}

TEST(insert_after_a_middle_node_relinks_the_successor)
{
    LinkedList l;
    LinkedList_init(&l);
    Item *a = item(1), *b = item(2), *x = item(9);
    LinkedList_append(&l, &a->n);
    LinkedList_append(&l, &b->n);
    LinkedList_insertAfter(&l, &x->n, &a->n);            /* a x b : b.prev must now be x */
    check_list(&l, (int[]){1, 9, 2}, 3);
    CHECK(b->n.prev == &x->n);
    LinkedList_freeEntries(&l);
}

TEST(a_node_queued_in_another_list_is_not_found_here)
{
    LinkedList mine, theirs;
    LinkedList_init(&mine);
    LinkedList_init(&theirs);
    Item *a = item(1), *b = item(2), *c = item(3);
    LinkedList_append(&mine, &a->n);
    LinkedList_append(&theirs, &b->n);
    LinkedList_append(&theirs, &c->n);                   /* b has a non-NULL next => looks "enqueued" */
    CHECK_EQ_INT(LinkedList_indexOf(&mine, &b->n), -1);  /* ...but is not in `mine` */
    CHECK(!LinkedList_contains(&mine, &b->n));
    LinkedList_freeEntries(&mine);
    LinkedList_freeEntries(&theirs);
}

TEST(inserting_into_an_empty_list_relative_to_a_foreign_anchor_adopts_the_ends)
{
    /* Defensive branches: the list being inserted into has no first/last yet. The anchor belongs to
     * another list, which is the only way to reach them; the point is that the target list ends up
     * with the new node as its end instead of dangling NULLs. */
    LinkedList target, donor;
    LinkedList_init(&target);
    LinkedList_init(&donor);
    Item *anchor = item(0), *before = item(1), *after = item(2);
    LinkedList_append(&donor, &anchor->n);

    LinkedList_insertBefore(&target, &before->n, &anchor->n);
    CHECK(target.first == &before->n);                   /* anchor had no predecessor => target head  */
    CHECK(target.last == &before->n);                    /* target was empty        => target tail    */
    CHECK(donor.first == &anchor->n);                    /* the donor list is not touched             */

    LinkedList tail_target;
    LinkedList_init(&tail_target);
    LinkedList_insertAfter(&tail_target, &after->n, &anchor->n);
    CHECK(tail_target.last == &after->n);                /* anchor had no successor => target tail    */
    CHECK(tail_target.first == &after->n);               /* target was empty        => target head    */
    CHECK(donor.last == &anchor->n);
    LinkedList_freeEntries(&donor);                      /* walks donor.first -> anchor -> after and frees both */
    afree(before);                                       /* only linked to `target`: not reachable from the donor */
}

TEST(invariant_check_detects_a_wrong_count)
{
    LinkedList l;
    LinkedList_init(&l);
    LinkedList_append(&l, &item(1)->n);
    LinkedList_append(&l, &item(2)->n);
    CHECK(LinkedList_invariantOk(&l));
    l.cnt = 3;                                           /* claims more nodes than are linked */
    CHECK(!LinkedList_invariantOk(&l));
    l.cnt = 1;                                           /* claims fewer */
    CHECK(!LinkedList_invariantOk(&l));
    l.cnt = 2;
    LinkedList_freeEntries(&l);
}
