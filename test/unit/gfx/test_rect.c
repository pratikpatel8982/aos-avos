/*
 * Source/rect.c -- integer rectangle geometry, driven through its public API (rect.h).
 * Real object code from libavos_cov.a; every expectation below is computed by hand.
 *
 * Convention under test: Rect = {x, y, width, height}; the pixel range is [x, x+width) and the
 * "FromCoords" form uses INCLUSIVE right/bottom edges.
 */
#include "avos_test.h"
#include "rect.h"
#include "tty.h"

static Rect R(int x, int y, int w, int h) { Rect r = { x, y, w, h }; return r; }

static void check_rect(Rect got, int x, int y, int w, int h)
{
    CHECK_EQ_INT(got.x, x);
    CHECK_EQ_INT(got.y, y);
    CHECK_EQ_INT(got.width, w);
    CHECK_EQ_INT(got.height, h);
}

TEST(set_stores_all_four_fields_including_negative_values)
{
    Rect r = { 99, 99, 99, 99 };
    Rect_Set(&r, -5, 7, 30, 40);
    check_rect(r, -5, 7, 30, 40);
    Rect_Set(&r, 0, 0, 0, 0);
    check_rect(r, 0, 0, 0, 0);
}

TEST(from_coords_uses_inclusive_right_and_bottom_edges)
{
    check_rect(Rect_FromCoords(0, 0, 0, 0), 0, 0, 1, 1);          /* a single pixel */
    check_rect(Rect_FromCoords(10, 20, 19, 49), 10, 20, 10, 30);
    check_rect(Rect_FromCoords(-3, -3, 3, 3), -3, -3, 7, 7);
    /* right < left gives a non-positive size: the caller's inverted input is not "fixed" */
    check_rect(Rect_FromCoords(5, 5, 2, 2), 5, 5, -2, -2);
    CHECK(!Rect_IsValid(&(Rect){5, 5, -2, -2}));
}

TEST(is_valid_requires_strictly_positive_width_and_height)
{
    Rect a = R(0, 0, 1, 1), b = R(0, 0, 0, 5), c = R(0, 0, 5, 0), d = R(0, 0, -1, 5), e = R(0, 0, 5, -1);
    Rect big = R(-100, -100, 0x7fffffff, 0x7fffffff);
    CHECK(Rect_IsValid(&a));
    CHECK(!Rect_IsValid(&b));
    CHECK(!Rect_IsValid(&c));
    CHECK(!Rect_IsValid(&d));
    CHECK(!Rect_IsValid(&e));
    CHECK(Rect_IsValid(&big));                /* position never matters for validity */
}

TEST(is_empty_is_true_only_for_a_zero_dimension_not_a_negative_one)
{
    Rect zw = R(3, 3, 0, 9), zh = R(3, 3, 9, 0), neg = R(3, 3, -1, 9), ok = R(3, 3, 1, 1);
    CHECK(Rect_IsEmpty(&zw));
    CHECK(Rect_IsEmpty(&zh));
    CHECK(Rect_IsEmpty(&(Rect){0, 0, 0, 0}));
    /* oddity (documented): a negative size is invalid but NOT "empty" */
    CHECK(!Rect_IsEmpty(&neg));
    CHECK(!Rect_IsValid(&neg));
    CHECK(!Rect_IsEmpty(&ok));
}

TEST(contains_point_is_half_open_on_the_right_and_bottom)
{
    Rect r = R(10, 20, 5, 3);                 /* x in [10,15) , y in [20,23) */
    CHECK(Rect_Contains(&r, 10, 20));         /* top-left corner is inside */
    CHECK(Rect_Contains(&r, 14, 22));         /* last pixel */
    CHECK(!Rect_Contains(&r, 15, 22));        /* one past right */
    CHECK(!Rect_Contains(&r, 14, 23));        /* one past bottom */
    CHECK(!Rect_Contains(&r, 9, 20));
    CHECK(!Rect_Contains(&r, 10, 19));
    CHECK(!Rect_Contains(&r, -1000, -1000));
    CHECK(!Rect_Contains(&r, 1000, 1000));
    Rect empty = R(10, 20, 0, 0);
    CHECK(!Rect_Contains(&empty, 10, 20));    /* nothing is inside an empty rect */
}

TEST(dist_squared_is_zero_inside_and_on_edges_and_euclidean_outside)
{
    Rect r = R(10, 10, 5, 5);                 /* inclusive pixel range 10..14 */
    CHECK_EQ_INT(Rect_DistSquared(&r, 12, 12), 0);
    CHECK_EQ_INT(Rect_DistSquared(&r, 10, 10), 0);
    CHECK_EQ_INT(Rect_DistSquared(&r, 14, 14), 0);   /* last inside pixel */
    CHECK_EQ_INT(Rect_DistSquared(&r, 15, 14), 1);   /* one right of the last pixel */
    CHECK_EQ_INT(Rect_DistSquared(&r, 14, 15), 1);   /* one below */
    CHECK_EQ_INT(Rect_DistSquared(&r, 9, 12), 1);    /* one left */
    CHECK_EQ_INT(Rect_DistSquared(&r, 12, 9), 1);    /* one above */
    CHECK_EQ_INT(Rect_DistSquared(&r, 5, 12), 25);   /* 5 left */
    CHECK_EQ_INT(Rect_DistSquared(&r, 20, 12), 36);  /* 6 right of pixel 14 */
    CHECK_EQ_INT(Rect_DistSquared(&r, 7, 6), 9 + 16);    /* diagonal top-left: 3^2 + 4^2 */
    CHECK_EQ_INT(Rect_DistSquared(&r, 17, 18), 9 + 16);  /* diagonal bottom-right */
    CHECK_EQ_INT(Rect_DistSquared(&r, 7, 18), 9 + 16);   /* bottom-left */
    CHECK_EQ_INT(Rect_DistSquared(&r, 17, 6), 9 + 16);   /* top-right */
}

TEST(intersects_is_false_for_touching_and_disjoint_and_true_for_any_overlap)
{
    Rect a = R(0, 0, 10, 10);
    CHECK(Rect_Intersects(&a, &(Rect){5, 5, 10, 10}));      /* partial overlap */
    CHECK(Rect_Intersects(&a, &(Rect){2, 2, 3, 3}));        /* b inside a */
    CHECK(Rect_Intersects(&a, &(Rect){-5, -5, 100, 100}));  /* a inside b */
    CHECK(Rect_Intersects(&a, &(Rect){9, 9, 1, 1}));        /* sharing only the last pixel */
    CHECK(!Rect_Intersects(&a, &(Rect){10, 0, 5, 5}));      /* edge-adjacent on the right */
    CHECK(!Rect_Intersects(&a, &(Rect){0, 10, 5, 5}));      /* edge-adjacent below */
    CHECK(!Rect_Intersects(&a, &(Rect){-5, 0, 5, 5}));      /* edge-adjacent on the left */
    CHECK(!Rect_Intersects(&a, &(Rect){0, -5, 5, 5}));      /* edge-adjacent above */
    CHECK(!Rect_Intersects(&a, &(Rect){50, 50, 5, 5}));
    /* symmetry */
    Rect b = R(5, 5, 10, 10);
    CHECK_EQ_INT(Rect_Intersects(&a, &b), Rect_Intersects(&b, &a));
}

TEST(intersection_returns_the_overlap_or_the_zero_rect)
{
    Rect a = R(0, 0, 10, 10);
    check_rect(Rect_Intersection(&a, &(Rect){5, 5, 10, 10}), 5, 5, 5, 5);
    check_rect(Rect_Intersection(&a, &(Rect){2, 3, 4, 4}), 2, 3, 4, 4);        /* contained */
    check_rect(Rect_Intersection(&a, &(Rect){-5, -5, 100, 100}), 0, 0, 10, 10); /* container */
    check_rect(Rect_Intersection(&a, &a), 0, 0, 10, 10);
    /* disjoint: l > r (or t > b) so the all-zero rect is returned */
    check_rect(Rect_Intersection(&a, &(Rect){20, 20, 5, 5}), 0, 0, 0, 0);
    check_rect(Rect_Intersection(&a, &(Rect){20, 2, 5, 5}), 0, 0, 0, 0);   /* only x disjoint */
    check_rect(Rect_Intersection(&a, &(Rect){2, 20, 5, 5}), 0, 0, 0, 0);   /* only y disjoint */
    /* edge-adjacent: l == r, so a zero-width rect at the shared edge (not the all-zero rect) */
    check_rect(Rect_Intersection(&a, &(Rect){10, 0, 5, 5}), 10, 0, 0, 5);
    Rect z = Rect_Intersection(&a, &(Rect){0, 10, 5, 5});
    check_rect(z, 0, 10, 5, 0);
    CHECK(Rect_IsEmpty(&z));
}

TEST(union_ignores_invalid_operands_and_otherwise_spans_both)
{
    Rect a = R(0, 0, 10, 10), b = R(20, 5, 5, 30);
    check_rect(Rect_Union(&a, &b), 0, 0, 25, 35);
    check_rect(Rect_Union(&b, &a), 0, 0, 25, 35);                     /* commutative */
    check_rect(Rect_Union(&a, &(Rect){2, 2, 3, 3}), 0, 0, 10, 10);    /* contained */
    check_rect(Rect_Union(&a, &(Rect){-4, -6, 2, 2}), -4, -6, 14, 16);
    Rect inval = R(100, 100, 0, 7), neg = R(100, 100, -3, 7);
    check_rect(Rect_Union(&inval, &b), 20, 5, 5, 30);                 /* invalid a -> b */
    check_rect(Rect_Union(&a, &inval), 0, 0, 10, 10);                 /* invalid b -> a */
    check_rect(Rect_Union(&neg, &inval), 100, 100, 0, 7);             /* both invalid: returns *rb */
    check_rect(Rect_Union(&inval, &neg), 100, 100, -3, 7);
}

TEST(adjusted_moves_each_edge_by_its_delta)
{
    Rect r = R(10, 20, 30, 40);               /* inclusive coords 10..39, 20..59 */
    check_rect(Rect_Adjusted(&r, 0, 0, 0, 0), 10, 20, 30, 40);
    check_rect(Rect_Adjusted(&r, 1, 2, 3, 4), 11, 22, 32, 42);         /* left+1 top+2, right+3 bottom+4 */
    check_rect(Rect_Adjusted(&r, -5, -5, 5, 5), 5, 15, 40, 50);        /* grow on all sides */
    check_rect(Rect_Adjusted(&r, 5, 5, -5, -5), 15, 25, 20, 30);       /* shrink on all sides */
    check_rect(Rect_Adjusted(&r, 30, 0, 0, 0), 40, 20, 0, 40);         /* left edge crosses the right edge */
}

TEST(contains_rect_checks_both_top_left_and_last_pixel_corners)
{
    Rect a = R(0, 0, 10, 10);
    CHECK(Rect_ContainsRect(&a, &a));
    CHECK(Rect_ContainsRect(&a, &(Rect){2, 2, 3, 3}));
    CHECK(Rect_ContainsRect(&a, &(Rect){9, 9, 1, 1}));
    CHECK(!Rect_ContainsRect(&a, &(Rect){9, 9, 2, 2}));      /* sticks out right+bottom */
    CHECK(!Rect_ContainsRect(&a, &(Rect){-1, 0, 5, 5}));     /* top-left outside */
    CHECK(!Rect_ContainsRect(&a, &(Rect){5, 5, 6, 1}));      /* only the right edge outside */
    CHECK(!Rect_ContainsRect(&a, &(Rect){20, 20, 2, 2}));
    CHECK(!Rect_ContainsRect(&(Rect){2, 2, 3, 3}, &a));      /* not symmetric */
}

TEST(equals_compares_every_field)
{
    Rect a = R(1, 2, 3, 4);
    CHECK(Rect_Equals(&a, &(Rect){1, 2, 3, 4}));
    CHECK(Rect_Equals(&a, &a));
    CHECK(!Rect_Equals(&a, &(Rect){0, 2, 3, 4}));
    CHECK(!Rect_Equals(&a, &(Rect){1, 0, 3, 4}));
    CHECK(!Rect_Equals(&a, &(Rect){1, 2, 0, 4}));
    CHECK(!Rect_Equals(&a, &(Rect){1, 2, 3, 0}));
}

TEST(fit_wider_image_is_letterboxed_to_destination_width)
{
    /* 1920x1080 (16:9) into 800x600 with square pixels: width fills, height = 800*1080/1920 = 450 */
    Rect src = R(0, 0, 1920, 1080), dst = R(0, 0, 800, 600);
    check_rect(Rect_Fit(&src, &dst, 1, 1), 0, (600 - 450) / 2, 800, 450);
    /* 2.35:1 into 1000x1000: height = 1000*100/235 = 425 (int division), y = (1000-425)/2 = 287 */
    check_rect(Rect_Fit(&(Rect){0, 0, 235, 100}, &(Rect){0, 0, 1000, 1000}, 1, 1), 0, 287, 1000, 425);
}

TEST(fit_taller_image_is_pillarboxed_to_destination_height)
{
    /* 480x640 into 800x600: height fills; width = 600*480/640 = 450 -> even, x = (800-450)/2 = 175 -> 174 */
    Rect src = R(0, 0, 480, 640), dst = R(0, 0, 800, 600);
    check_rect(Rect_Fit(&src, &dst, 1, 1), 174, 0, 450, 600);
    /* 4:3 into 16:9 (1280x720): width = 720*4/3 = 960, x = 160 */
    check_rect(Rect_Fit(&(Rect){0, 0, 640, 480}, &(Rect){0, 0, 1280, 720}, 1, 1), 160, 0, 960, 720);
}

TEST(fit_identical_aspect_takes_the_pillarbox_branch_and_fills_the_destination)
{
    /* equal ratio: the strict '>' test is false, so the height-driven branch runs */
    check_rect(Rect_Fit(&(Rect){0, 0, 640, 360}, &(Rect){0, 0, 1280, 720}, 1, 1), 0, 0, 1280, 720);
}

TEST(fit_applies_the_pixel_aspect_ratio_and_rounds_x_and_width_to_even)
{
    /* anamorphic 720x576 with PAR 16/15 into 1024x768:
     * wide test: 720*768*15 = 8294400 > 1024*576*16 = 9437184 ? no -> height branch
     * height=768, width = 768*720*15/(576*16) = 8294400/9216 = 900, x = (1024-900)/2 = 62 */
    check_rect(Rect_Fit(&(Rect){0, 0, 720, 576}, &(Rect){0, 0, 1024, 768}, 16, 15), 62, 0, 900, 768);
    /* PAR 2:1 on a 100x100 image into 300x300: displayed 200x100 -> wide branch.
     * width=300, height = 300*100*2/(100*1) = 600?? no: par_n=2, par_d=1 -> height = w*h*n/(w*d) = 600 */
    Rect r = Rect_Fit(&(Rect){0, 0, 100, 100}, &(Rect){0, 0, 300, 300}, 2, 1);
    /* 100*300*1 = 30000 > 300*100*2 = 60000 ? no -> height branch: height 300, width = 300*100*1/(100*2) = 150, x = 75 -> 74 */
    check_rect(r, 74, 0, 150, 300);
    /* odd width is rounded down to even: 3x1 into 7x7: 3*7*1 > 7*1*1 -> wide: width 7 -> 6, height = 7*1/3 = 2, y = 2 */
    check_rect(Rect_Fit(&(Rect){0, 0, 3, 1}, &(Rect){0, 0, 7, 7}, 1, 1), 0, 2, 6, 2);
}

TEST(dump_writes_the_tag_and_numbers_to_the_debug_console)
{
    /* serprintf -> TTY_write -> stdout (only once TTY_open ran); capture stdout of a child through a pipe */
    int fd[2];
    REQUIRE(pipe(fd) == 0);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        TTY_open(NULL);        /* serprintf -> TTY_write is a no-op until the tty layer is open */
        Rect r = R(12, 34, 56, 78);
        Rect_Dump(&r, "mytag");
        fflush(NULL);
        __gcov_dump();         /* _exit() skips gcov's atexit flush; without it these lines are not counted */
        _exit(0);
    }
    close(fd[1]);
    char buf[512] = { 0 };
    ssize_t n = 0, got;                      /* read to EOF: the child may write a warning first, in a separate chunk */
    while (n < (ssize_t)sizeof buf - 1 && (got = read(fd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += got;
    close(fd[0]);
    waitpid(pid, NULL, 0);
    REQUIRE(n > 0);
    CHECK(strstr(buf, "RECT[mytag]") != NULL);
    CHECK(strstr(buf, "  12x 34") != NULL);
    CHECK(strstr(buf, "  56x 78") != NULL);
}
