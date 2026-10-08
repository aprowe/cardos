/* The pet's life, apart from the screen: what time does to it and what the
 * keys do for it. Header-only and libc-free, like apps/forklang.h, so the
 * app and the host tests (test/test_petsim.c) share one copy.
 *
 * Time is wall-clock minutes. The app calls pet_advance with the epoch each
 * time it opens and once a minute while it is open, so a pet left for a day
 * has had a day: it got hungry, made a mess, slept when it was tired, and
 * may be ill. Stats run 0..100, 100 the good end. Nothing here is random
 * except what the caller hands in (pet_seed).
 *
 * Stages go by age -- egg, baby, child, teen, adult -- and the adult it
 * grows into depends on how many times it was let down (a stat at zero, the
 * floor left messy): few is a bright one, many a scruffy one. Long neglect
 * -- starving and ill for a day together -- and it leaves. A new egg is one
 * key away; nothing is lost but the pet.
 */
#ifndef CARDOS_PETSIM_H
#define CARDOS_PETSIM_H

#include <stdint.h>

enum { PET_EGG = 0, PET_BABY, PET_CHILD, PET_TEEN, PET_ADULT, PET_GONE };
enum { PET_FORM_BRIGHT = 0, PET_FORM_PLAIN, PET_FORM_SCRUFFY };

#define PET_EGG_MIN      3            /* hatches after this many minutes */
#define PET_BABY_MIN     (24 * 60)
#define PET_CHILD_MIN    (3 * 24 * 60)
#define PET_TEEN_MIN     (6 * 24 * 60)
#define PET_POOP_MAX     4
#define PET_POOP_EVERY   100          /* awake minutes between messes */
#define PET_GONE_AFTER   (24 * 60)    /* starving and ill, together, this long */
#define PET_CATCHUP_MAX  (14 * 24 * 60)
#define PET_HUNGRY_AT    25           /* when it calls for food */

typedef struct {
  uint32_t born, last;     /* epoch seconds; last is how far time has been applied */
  int age_min;             /* minutes lived (awake or asleep) */
  int food, fun, energy;   /* 0..100 */
  int poop;                /* 0..PET_POOP_MAX */
  int sick;                /* 0 or 1 */
  int asleep;
  int weight;              /* grams, as on the real thing; 5 at hatching */
  int misses;              /* times let down: shapes the adult */
  int form;                /* PET_FORM_*, fixed when it becomes an adult */
  int stage;
  /* running counts, kept so time applies the same in one go or in pieces */
  int awake_min, mess_min, starve_min, dirty_min;
  int low_food, low_fun;   /* 1 while that stat sits at zero (one miss each time) */
  uint32_t seed;
} Pet;

static inline int pet_clamp(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static inline uint32_t pet_rand(Pet *p) {
  p->seed = p->seed * 1103515245u + 12345u;
  return (p->seed >> 16) & 0x7FFF;
}

static inline void pet_new(Pet *p, uint32_t now, uint32_t seed) {
  /* By hand: an app has no memset, and `= {0}` or a struct copy calls one.
   * volatile, or the compiler turns the loop back into that call. */
  volatile unsigned char *b = (volatile unsigned char *)p;
  unsigned i;
  for (i = 0; i < sizeof *p; i++) b[i] = 0;
  p->born = p->last = now;
  p->food = 80; p->fun = 80; p->energy = 100;
  p->weight = 5;
  p->stage = PET_EGG;
  p->form = PET_FORM_PLAIN;
  p->seed = seed ? seed : 1;
}

static inline int pet_stage_for(int age_min) {
  if (age_min < PET_EGG_MIN) return PET_EGG;
  if (age_min < PET_BABY_MIN) return PET_BABY;
  if (age_min < PET_CHILD_MIN) return PET_CHILD;
  if (age_min < PET_TEEN_MIN) return PET_TEEN;
  return PET_ADULT;
}

/* One minute of life. */
static inline void pet_minute(Pet *p) {
  int m;
  if (p->stage == PET_GONE) return;
  m = ++p->age_min;
  if (p->stage == PET_EGG) {
    if (pet_stage_for(m) != PET_EGG) p->stage = PET_BABY;
    return;
  }
  if (p->asleep) {
    if (m % 8 == 0) p->food--;
    if (m % 2 == 0) p->energy++;
    if (p->energy >= 100) { p->energy = 100; p->asleep = 0; }   /* wakes rested */
  } else {
    p->awake_min++;
    if (m % 4 == 0) p->food--;
    if (m % (p->sick ? 3 : 6) == 0) p->fun--;
    if (m % 7 == 0) p->energy--;
    if (p->awake_min % PET_POOP_EVERY == 0 && p->poop < PET_POOP_MAX) p->poop++;
    /* Too tired to stay up: it drops off by itself (lights are the
     * player's courtesy, not a requirement). */
    if (p->energy <= 0) p->asleep = 1;
  }
  p->food = pet_clamp(p->food);
  p->fun = pet_clamp(p->fun);
  p->energy = pet_clamp(p->energy);

  /* Let-downs: each time a stat reaches zero, and each hour the floor is full. */
  if (!p->food && !p->low_food) { p->low_food = 1; p->misses++; }
  if (p->food) p->low_food = 0;
  if (!p->fun && !p->low_fun) { p->low_fun = 1; p->misses++; }
  if (p->fun) p->low_fun = 0;
  if (p->poop >= PET_POOP_MAX) { if (++p->dirty_min % 60 == 0) p->misses++; }
  else p->dirty_min = 0;

  /* Ill from a dirty floor or an empty stomach, kept up for a while. */
  p->mess_min = p->poop >= 3 ? p->mess_min + 1 : 0;
  p->starve_min = p->food == 0 ? p->starve_min + 1 : 0;
  if (p->mess_min >= 90 || p->starve_min >= 180) p->sick = 1;
  if (p->sick && p->starve_min >= PET_GONE_AFTER) { p->stage = PET_GONE; return; }

  {
    int st = pet_stage_for(m);
    if (st > p->stage) {
      p->stage = st;
      if (st == PET_ADULT)
        p->form = p->misses <= 3 ? PET_FORM_BRIGHT : p->misses <= 10 ? PET_FORM_PLAIN
                                                                    : PET_FORM_SCRUFFY;
    }
  }
}

/* Time to `now`. A clock that went backwards changes nothing; a gap longer
 * than two weeks counts as two weeks. Returns the minutes applied. */
static inline int pet_advance(Pet *p, uint32_t now) {
  uint32_t mins;
  int i;
  if (!now || now <= p->last) return 0;
  mins = (now - p->last) / 60u;
  if (!mins) return 0;
  p->last += mins * 60u;
  if (mins > PET_CATCHUP_MAX) mins = PET_CATCHUP_MAX;
  for (i = 0; i < (int)mins && p->stage != PET_GONE; i++) pet_minute(p);
  return (int)mins;
}

/* ---- what the player does. Each returns 0 or a short reason it did not. */

static inline const char *pet_awake_reason(const Pet *p) {
  if (p->stage == PET_GONE) return "it has gone";
  if (p->stage == PET_EGG) return "it is still an egg";
  if (p->asleep) return "it is asleep";
  return 0;
}

static inline const char *pet_feed(Pet *p) {
  const char *why = pet_awake_reason(p);
  if (why) return why;
  if (p->food >= 95) return "it is full";
  p->food = pet_clamp(p->food + 30);
  p->weight += 1;
  return 0;
}

static inline const char *pet_snack(Pet *p) {
  const char *why = pet_awake_reason(p);
  if (why) return why;
  p->fun = pet_clamp(p->fun + 15);
  p->food = pet_clamp(p->food + 5);
  p->weight += 2;
  return 0;
}

/* After the game: `wins` of 5 rounds. Playing tires it whatever happens. */
static inline void pet_played(Pet *p, int wins) {
  p->energy = pet_clamp(p->energy - 8);
  if (wins >= 3) { p->fun = pet_clamp(p->fun + 25); if (p->weight > 5) p->weight--; }
  else p->fun = pet_clamp(p->fun + 5);
}

static inline const char *pet_can_play(const Pet *p) {
  const char *why = pet_awake_reason(p);
  if (why) return why;
  if (p->sick) return "it is too ill to play";
  if (p->energy < 10) return "it is too tired";
  return 0;
}

static inline const char *pet_clean(Pet *p) {
  if (p->stage == PET_GONE) return "it has gone";
  if (!p->poop) return "it is already clean";
  p->poop = 0;
  p->mess_min = 0;
  return 0;
}

static inline const char *pet_medicine(Pet *p) {
  if (p->stage == PET_GONE) return "it has gone";
  if (!p->sick) return "it is not ill";
  p->sick = 0;
  p->starve_min = 0;
  p->mess_min = 0;
  p->fun = pet_clamp(p->fun - 10);   /* it does not like the taste */
  return 0;
}

/* Lights out puts it to bed; lights on wakes it (a little grumpy). */
static inline const char *pet_lights(Pet *p) {
  if (p->stage == PET_GONE) return "it has gone";
  if (p->stage == PET_EGG) return "it is still an egg";
  if (p->asleep) { p->asleep = 0; p->fun = pet_clamp(p->fun - 5); }
  else p->asleep = 1;
  return 0;
}

/* Minutes until it gets hungry, for the reminder; -1 if it never will
 * without something changing (an egg, gone, or already hungry). Counted at
 * the awake rate: sleeping only puts it off. */
static inline int pet_minutes_to_hungry(const Pet *p) {
  if (p->stage == PET_GONE || p->stage == PET_EGG || p->food <= PET_HUNGRY_AT) return -1;
  return (p->food - PET_HUNGRY_AT) * 4;
}

/* Its mood in a word, the worst thing first. */
static inline const char *pet_mood(const Pet *p) {
  if (p->stage == PET_GONE) return "gone";
  if (p->stage == PET_EGG) return "an egg";
  if (p->sick) return "ill";
  if (p->asleep) return "asleep";
  if (p->food <= PET_HUNGRY_AT) return "hungry";
  if (p->poop >= 2) return "messy";
  if (p->fun <= 25) return "bored";
  if (p->energy <= 20) return "sleepy";
  if (p->food >= 70 && p->fun >= 70) return "happy";
  return "fine";
}

#endif /* CARDOS_PETSIM_H */
