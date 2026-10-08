/* The pet's life on the host: apps/petsim.h. */
#include "tinytest.h"
#include "apps/petsim.h"

#define T0 1800000000u

void test_petsim_hatches_and_gets_hungry_while_away(void) {
  Pet p;
  pet_new(&p, T0, 7);
  CHECK_EQ(p.stage, PET_EGG);
  CHECK(pet_feed(&p) != 0);                    /* an egg does not eat */
  CHECK_EQ(pet_advance(&p, T0 + 5 * 60), 5);
  CHECK_EQ(p.stage, PET_BABY);
  /* four hours away: hungry, bored, a mess on the floor */
  pet_advance(&p, T0 + 5 * 60 + 4 * 3600);
  CHECK(p.food < 30);
  CHECK(p.poop >= 2);
  CHECK(pet_minutes_to_hungry(&p) == -1 || p.food > PET_HUNGRY_AT);
}

void test_petsim_time_in_pieces_is_time_in_one_go(void) {
  Pet a, b;
  int i;
  pet_new(&a, T0, 3);
  pet_new(&b, T0, 3);
  pet_advance(&a, T0 + 10 * 3600);
  for (i = 1; i <= 600; i++) pet_advance(&b, T0 + (uint32_t)i * 60);
  CHECK_EQ(a.food, b.food);
  CHECK_EQ(a.fun, b.fun);
  CHECK_EQ(a.energy, b.energy);
  CHECK_EQ(a.poop, b.poop);
  CHECK_EQ(a.asleep, b.asleep);
  CHECK_EQ(a.age_min, b.age_min);
  /* seconds carry: two thirty-second calls are one minute */
  pet_new(&a, T0, 3);
  CHECK_EQ(pet_advance(&a, T0 + 30), 0);
  CHECK_EQ(pet_advance(&a, T0 + 60), 1);
  /* a clock that went back changes nothing */
  CHECK_EQ(pet_advance(&a, T0), 0);
}

void test_petsim_care(void) {
  Pet p;
  pet_new(&p, T0, 1);
  pet_advance(&p, T0 + 10 * 60);
  p.food = 50;
  CHECK(pet_feed(&p) == 0);
  CHECK_EQ(p.food, 80);
  p.food = 96;
  CHECK(pet_feed(&p) != 0);                    /* full */
  p.poop = 3;
  CHECK(pet_clean(&p) == 0);
  CHECK_EQ(p.poop, 0);
  CHECK(pet_clean(&p) != 0);
  CHECK(pet_medicine(&p) != 0);                /* not ill */
  p.sick = 1;
  CHECK(pet_can_play(&p) != 0);
  CHECK(pet_medicine(&p) == 0);
  CHECK_EQ(p.sick, 0);
  CHECK(pet_lights(&p) == 0);
  CHECK(p.asleep);
  CHECK(pet_feed(&p) != 0);                    /* asleep */
  /* asleep, it rests and wakes by itself */
  p.energy = 50;
  pet_advance(&p, T0 + 10 * 60 + 3 * 3600);
  CHECK_EQ(p.asleep, 0);
  pet_played(&p, 4);
  CHECK(p.fun > 0);
}

void test_petsim_neglect_makes_it_ill_then_it_leaves(void) {
  Pet p;
  pet_new(&p, T0, 9);
  pet_advance(&p, T0 + 3 * 24 * 3600);
  CHECK(p.sick);
  CHECK(p.misses > 0);
  pet_advance(&p, T0 + 6 * 24 * 3600);
  CHECK_EQ(p.stage, PET_GONE);
  CHECK(pet_feed(&p) != 0);
  CHECK_EQ(pet_advance(&p, T0 + 7 * 24 * 3600) > 0, 1);   /* time passes; nothing happens */
  CHECK_EQ(p.stage, PET_GONE);
}

void test_petsim_well_kept_grows_bright(void) {
  Pet p;
  uint32_t t = T0;
  int h;
  pet_new(&p, T0, 5);
  /* an attentive owner: every two hours, everything it needs */
  for (h = 0; h < 7 * 12; h++) {
    t += 2 * 3600;
    pet_advance(&p, t);
    if (p.asleep && p.energy > 60) pet_lights(&p);
    while (p.food < 80 && !pet_feed(&p)) {}
    pet_clean(&p);
    pet_medicine(&p);
    if (!pet_can_play(&p)) pet_played(&p, 5);
    p.fun = pet_clamp(p.fun + 40);             /* and plenty of games */
  }
  CHECK_EQ(p.stage, PET_ADULT);
  CHECK_EQ(p.form, PET_FORM_BRIGHT);
  CHECK_EQ(p.sick, 0);
}
