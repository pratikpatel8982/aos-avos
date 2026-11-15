/*
 * Covers Source/xdm_utils.c: the frame-id -> (type, time) history ring and the timestamp FIFO used by
 * the XDM decoder glue. Real object code from libavos_cov.a.
 */
#include "avos_test.h"
#include "xdm_utils.h"

static void fresh(struct XDM_ctx *c)
{
    memset(c, 0xAB, sizeof *c);
    XDM_id_flush(c);
    XDM_ts_flush(c);
}

TEST(id_flush_clears_every_slot_and_resets_cursor)
{
    struct XDM_ctx c;
    memset(&c, 0x55, sizeof c);
    XDM_id_flush(&c);
    CHECK_EQ_INT(c.id_now, 0);
    for (int i = 0; i < ID_MAX; i++) {
        CHECK_EQ_INT(c.itf[i].id, -1);
        CHECK_EQ_INT(c.itf[i].type, -1);
        CHECK_EQ_INT(c.itf[i].time, -1);
    }
}

TEST(id_get_on_empty_history_reports_not_found_and_resets_outputs)
{
    struct XDM_ctx c;
    fresh(&c);
    int type = 7, time = 9;
    CHECK_EQ_INT(XDM_id_get(&c, 5, &type, &time), 1);
    CHECK_EQ_INT(type, -1);
    CHECK_EQ_INT(time, -1);
}

TEST(id_get_not_found_tolerates_null_output_pointers)
{
    struct XDM_ctx c;
    fresh(&c);
    CHECK_EQ_INT(XDM_id_get(&c, 5, NULL, NULL), 1);
}

TEST(id_put_then_get_returns_stored_type_and_time)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_id_put(&c, 100, 3, 4000);
    XDM_id_put(&c, 101, 2, 4040);
    XDM_id_put(&c, 102, 1, 4080);
    int type = 0, time = 0;
    CHECK_EQ_INT(XDM_id_get(&c, 101, &type, &time), 0);
    CHECK_EQ_INT(type, 2);
    CHECK_EQ_INT(time, 4040);
    CHECK_EQ_INT(XDM_id_get(&c, 100, &type, &time), 0);
    CHECK_EQ_INT(type, 3);
    CHECK_EQ_INT(time, 4000);
    CHECK_EQ_INT(XDM_id_get(&c, 102, &type, &time), 0);
    CHECK_EQ_INT(time, 4080);
    CHECK_EQ_INT(XDM_id_get(&c, 103, &type, &time), 1);
}

TEST(id_get_found_tolerates_null_output_pointers)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_id_put(&c, 8, 1, 2);
    CHECK_EQ_INT(XDM_id_get(&c, 8, NULL, NULL), 0);
    int t = 0;
    CHECK_EQ_INT(XDM_id_get(&c, 8, &t, NULL), 0);
    CHECK_EQ_INT(t, 1);
    int tm = 0;
    CHECK_EQ_INT(XDM_id_get(&c, 8, NULL, &tm), 0);
    CHECK_EQ_INT(tm, 2);
}

TEST(id_get_prefers_the_most_recent_entry_with_a_duplicate_id)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_id_put(&c, 5, 1, 111);
    XDM_id_put(&c, 6, 1, 222);
    XDM_id_put(&c, 5, 2, 333);
    int type = 0, time = 0;
    CHECK_EQ_INT(XDM_id_get(&c, 5, &type, &time), 0);
    CHECK_EQ_INT(type, 2);
    CHECK_EQ_INT(time, 333);
}

TEST(id_history_wraps_after_id_max_and_overwrites_oldest)
{
    struct XDM_ctx c;
    fresh(&c);
    for (int i = 0; i < ID_MAX + 3; i++)
        XDM_id_put(&c, 1000 + i, i, i * 10);
    CHECK_EQ_INT(c.id_now, 3);                       /* wrapped */
    int type, time;
    CHECK_EQ_INT(XDM_id_get(&c, 1000, &type, &time), 1);   /* overwritten */
    CHECK_EQ_INT(XDM_id_get(&c, 1002, &type, &time), 1);
    /* 1003 still sits in slot 3 == id_now, but the search stops at the cursor, so it is not reported
     * (current behaviour: the ring effectively holds ID_MAX-1 searchable entries). */
    CHECK_EQ_INT(XDM_id_get(&c, 1003, &type, &time), 1);
    CHECK_EQ_INT(type, -1);
    CHECK_EQ_INT(time, -1);
    CHECK_EQ_INT(XDM_id_get(&c, 1004, &type, &time), 0);   /* oldest reachable survivor */
    CHECK_EQ_INT(type, 4);
    CHECK_EQ_INT(time, 40);
    CHECK_EQ_INT(XDM_id_get(&c, 1000 + ID_MAX + 2, &type, &time), 0);   /* newest */
    CHECK_EQ_INT(time, (ID_MAX + 2) * 10);
}

TEST(id_get_searches_backwards_across_the_wrap_point)
{
    struct XDM_ctx c;
    fresh(&c);
    c.id_now = 1;                  /* newest slot is index 0, older ones are at the end of the array */
    c.itf[0].id = 1; c.itf[0].type = 10; c.itf[0].time = 100;
    c.itf[ID_MAX - 1].id = 2; c.itf[ID_MAX - 1].type = 20; c.itf[ID_MAX - 1].time = 200;
    int type = 0, time = 0;
    CHECK_EQ_INT(XDM_id_get(&c, 2, &type, &time), 0);
    CHECK_EQ_INT(type, 20);
    CHECK_EQ_INT(time, 200);
    CHECK_EQ_INT(XDM_id_get(&c, 1, &type, &time), 0);
    CHECK_EQ_INT(time, 100);
}

TEST(id_get_with_cursor_at_zero_starts_from_last_slot)
{
    struct XDM_ctx c;
    fresh(&c);
    for (int i = 0; i < ID_MAX; i++)
        XDM_id_put(&c, i + 1, 0, i);                 /* fills all slots, cursor back to 0 */
    CHECK_EQ_INT(c.id_now, 0);
    int type, time;
    CHECK_EQ_INT(XDM_id_get(&c, ID_MAX, &type, &time), 0);
    CHECK_EQ_INT(time, ID_MAX - 1);
    CHECK_EQ_INT(XDM_id_get(&c, 2, &type, &time), 0);          /* slot 1: reachable */
    CHECK_EQ_INT(time, 1);
    CHECK_EQ_INT(XDM_id_get(&c, 1, &type, &time), 1);          /* slot 0 == id_now: not examined */
    CHECK_EQ_INT(time, -1);
}

/* The search loop stops when idx reaches id_now, so the slot at the cursor (the oldest) is never examined. */
TEST(id_get_never_examines_the_slot_under_the_write_cursor)
{
    struct XDM_ctx c;
    fresh(&c);
    for (int i = 0; i < ID_MAX; i++)
        XDM_id_put(&c, i + 1, 0, i);
    int type, time;
    CHECK_EQ_INT(XDM_id_get(&c, 1, &type, &time), 1);         /* id 1 lives in slot 0 == id_now: skipped */
    XDM_id_put(&c, 99, 0, 99);                                /* cursor -> 1, slot 0 overwritten */
    CHECK_EQ_INT(XDM_id_get(&c, 2, &type, &time), 1);         /* id 2 sits at slot 1 == cursor: unreachable */
    CHECK_EQ_INT(XDM_id_get(&c, 3, &type, &time), 0);         /* its neighbour is still found */
    CHECK_EQ_INT(XDM_id_get(&c, 99, &type, &time), 0);        /* newly written slot 0 */
    CHECK_EQ_INT(time, 99);
}

TEST(ts_get_on_empty_fifo_returns_minus_one)
{
    struct XDM_ctx c;
    fresh(&c);
    CHECK_EQ_INT(XDM_ts_get(&c), -1);
}

TEST(ts_fifo_returns_values_in_insertion_order)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_ts_put(&c, 10);
    XDM_ts_put(&c, 20);
    XDM_ts_put(&c, 30);
    CHECK_EQ_INT(XDM_ts_get(&c), 10);
    CHECK_EQ_INT(XDM_ts_get(&c), 20);
    XDM_ts_put(&c, 40);
    CHECK_EQ_INT(XDM_ts_get(&c), 30);
    CHECK_EQ_INT(XDM_ts_get(&c), 40);
    CHECK_EQ_INT(XDM_ts_get(&c), -1);
}

TEST(ts_fifo_holds_at_most_ts_max_minus_one_and_drops_the_rest)
{
    struct XDM_ctx c;
    fresh(&c);
    for (int i = 0; i < TS_MAX + 10; i++)
        XDM_ts_put(&c, i);
    for (int i = 0; i < TS_MAX - 1; i++)
        CHECK_EQ_INT(XDM_ts_get(&c), i);
    CHECK_EQ_INT(XDM_ts_get(&c), -1);               /* the overflowing puts were dropped */
}

TEST(ts_fifo_wraps_around_the_array_end)
{
    struct XDM_ctx c;
    fresh(&c);
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 20; i++) XDM_ts_put(&c, round * 100 + i);
        for (int i = 0; i < 20; i++) CHECK_EQ_INT(XDM_ts_get(&c), round * 100 + i);
    }
    CHECK(c.ts_read < TS_MAX && c.ts_write < TS_MAX);
    CHECK_EQ_INT(c.ts_read, c.ts_write);
}

TEST(ts_flush_discards_pending_values)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_ts_put(&c, 1);
    XDM_ts_put(&c, 2);
    XDM_ts_flush(&c);
    CHECK_EQ_INT(c.ts_read, 0);
    CHECK_EQ_INT(c.ts_write, 0);
    CHECK_EQ_INT(XDM_ts_get(&c), -1);
}

TEST(ts_can_carry_negative_values_other_than_the_empty_marker)
{
    struct XDM_ctx c;
    fresh(&c);
    XDM_ts_put(&c, -5);
    XDM_ts_put(&c, 0);
    CHECK_EQ_INT(XDM_ts_get(&c), -5);
    CHECK_EQ_INT(XDM_ts_get(&c), 0);
}
