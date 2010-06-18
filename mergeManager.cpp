/*
 * mergeManager.cpp
 *
 *  Created on: May 19, 2010
 *      Author: sears
 */

#include "mergeManager.h"
#include "mergeStats.h"
#include "logstore.h"
#include "math.h"
mergeStats* mergeManager:: get_merge_stats(int mergeLevel) {
  if (mergeLevel == 0) {
    return c0;
  } else if (mergeLevel == 1) {
    return c1;
  } else if(mergeLevel == 2) {
    return c2;
  } else {
    abort();
  }
}

mergeManager::~mergeManager() {
  pthread_mutex_destroy(&throttle_mut);
  pthread_mutex_destroy(&dummy_throttle_mut);
  pthread_cond_destroy(&dummy_throttle_cond);
  pthread_cond_destroy(&throttle_wokeup_cond);
  delete c0;
  delete c1;
  delete c2;
}

void mergeManager::new_merge(int mergeLevel) {
  mergeStats * s = get_merge_stats(mergeLevel);
  if(s->merge_level == 0) {
    // target_size was set during startup
  } else if(s->merge_level == 1) {
    assert(c0->target_size);
    c1->target_size = (pageid_t)(*ltable->R() * (double)c0->target_size);
    assert(c1->target_size);
  } else if(s->merge_level == 2) {
    // target_size is infinity...
  } else { abort(); }
  s->new_merge2();
}
void mergeManager::set_c0_size(int64_t size) {
  c0->target_size = size;
}

static const int UPDATE_PROGRESS_DELTA = 1 * 1024 * 1024;

void mergeManager::update_progress(mergeStats * s, int delta) {
  s->delta += delta;
  if((!delta) || s->delta > UPDATE_PROGRESS_DELTA) { //512 * 1024) {
    if(delta) {
      rwlc_writelock(ltable->header_mut);
      s->delta = 0;
      s->need_tick = true;
    }
    if(s->merge_level != 0) {
      if(s->active) {
        s->in_progress =  ((double)(s->bytes_in_large + s->bytes_in_small)) / (double)(get_merge_stats(s->merge_level-1)->mergeable_size + s->base_size);
      } else {
        s->in_progress = 0;
      }
    }
    if(s->merge_level != 2) {
      if(s->mergeable_size) {
        s->out_progress = ((double)s->current_size) / (double)s->target_size;
      } else {
        s->out_progress = 0;
      }
    }
    s->current_size = s->base_size + s->bytes_out - s->bytes_in_large;
    struct timeval now;
    gettimeofday(&now, 0);
    double elapsed_delta = tv_to_double(&now) - ts_to_double(&s->last_tick);

    s->lifetime_elapsed += elapsed_delta;
    s->lifetime_consumed += s->bytes_in_small_delta;
    double decay = 0.9999; // XXX set this in some principled way.  Surely, it should depend on tick_length (once that's working...)
    s->window_elapsed = (decay * s->window_elapsed) + elapsed_delta;
    s->window_consumed = (decay * s->window_consumed) + s->bytes_in_small_delta;

    double_to_ts(&s->last_tick, tv_to_double(&now));

    s->bytes_in_small_delta = 0;

    s->bps = s->window_consumed / s->window_elapsed;

    if(delta) rwlc_unlock(ltable->header_mut);

  }
}

/**
 * This function is invoked periodically by the merge threads.  It updates mergeManager's statistics, and applies
 * backpressure as necessary.
 *
 * Here is the backpressure algorithm.
 *
 * We want to maintain these two invariants:
 *   - for each byte consumed by the app->c0 threads, a byte is consumed by the c0->c1 merge thread.
 *   - for each byte consumed by the c0->c1 thread, the c1->c2 thread consumes a byte
 *
 * More concretely (and taking into account varying values of R):
 *   capacity(C_i) - current_size(C_i) >= size(C_i_mergeable) - bytes_consumed_by_next_merger
 *
 * where:
 *   capacity c0 = c0_queue_size
 *   capacity c1 = c1_queue_size
 *
 *   current_size(c_i) = sum(bytes_out_delta) - sum(bytes_in_large_delta)
 *
 * bytes_consumed_by_merger = sum(bytes_in_small_delta)
 */
void mergeManager::tick(mergeStats * s, bool block, bool force) {
#define PRINT_SKIP 1000

  if(force || s->need_tick) {

    if(block) {
      rwlc_writelock(ltable->header_mut);
      s->need_tick = false;
      while(sleeping[s->merge_level]) {
        rwlc_cond_wait(&throttle_wokeup_cond, ltable->header_mut);
      }

      int64_t overshoot = 0;
      int64_t raw_overshoot = 0;
//      int64_t overshoot_fudge = (int64_t)((s->out_progress-0.5) * 18.0 * 1024.0 * 1024.0);
      double skew = 0.1; // should be between 0 and 0.5.  0 means that there is no 'catch up' after merge commit
      int64_t overshoot_fudge = (int64_t)((s->out_progress-skew) * (6 / (1.0-skew)) * 1024.0 * 1024.0);  // should be some function of R, delta interval, and force interval.
      int spin = 0;
      double total_sleep = 0.0;
      do{
        overshoot = 0;
        raw_overshoot = 0;
        double bps;
        // This needs to be here (and not in update_progress), since the other guy's in_progress changes while we sleep.
        if(s->merge_level == 0) {
          if(!(c1->active && c0->mergeable_size)) { overshoot_fudge = 0; }
          raw_overshoot = (int64_t)(((double)c0->target_size) * (c0->out_progress - c1->in_progress));
          overshoot = raw_overshoot + overshoot_fudge;
          bps = c1->bps;
        } else if (s->merge_level == 1) {
          if(!(c2->active && c1->mergeable_size)) { overshoot_fudge = 0; }
          raw_overshoot = (int64_t)(((double)c1->target_size) * (c1->out_progress - c2->in_progress));
          overshoot = raw_overshoot + overshoot_fudge;
          bps = c2->bps;
        }

//#define PP_THREAD_INFO
  #ifdef PP_THREAD_INFO
        printf("\nMerge thread %d %6f %6f Overshoot: raw=%lld, d=%lld eff=%lld Throttle min(1, %6f) spin %d, total_sleep %6.3f\n", s->merge_level, c0_out_progress, c0_c1_in_progress, raw_overshoot, overshoot_fudge, overshoot, -1.0, spin, total_sleep);
  #endif
        if(s->print_skipped == PRINT_SKIP) {
          pretty_print(stdout);
          s->print_skipped = 0;
        } else {
          s->print_skipped++;
        }
        if(overshoot > 0 && (raw_overshoot > 0 || total_sleep < 1)) {
          // throttle
          // it took "elapsed" seconds to process "tick_length_bytes" mb
          double sleeptime = 2.0 * (double)overshoot / bps;

          struct timespec sleep_until;

          double max_c0_sleep = 0.1;
          double min_c0_sleep = 0.05;
          double max_c1_sleep = 0.1;
          double min_c1_sleep = 0.1;
          double max_sleep = s->merge_level == 0 ? max_c0_sleep : max_c1_sleep;
          double min_sleep = s->merge_level == 0 ? min_c0_sleep : min_c1_sleep;

          if(sleeptime < min_sleep) { sleeptime = min_sleep; }
          if(sleeptime > max_sleep) { sleeptime = max_sleep; }

          spin ++;
          total_sleep += sleeptime;

          if((spin > 40) || (total_sleep > (max_sleep * 20.0))) {
//            if(spin > 20 || s->merge_level == 0) {
              printf("\nMerge thread %d Overshoot: raw=%lld, d=%lld eff=%lld Throttle min(1, %6f) spin %d, total_sleep %6.3f\n", s->merge_level, raw_overshoot, overshoot_fudge, overshoot, sleeptime, spin, total_sleep);
//            }
          }

          struct timeval now;
          gettimeofday(&now, 0);

          double_to_ts(&sleep_until, sleeptime + tv_to_double(&now));
          sleeping[s->merge_level] = true;
          rwlc_cond_timedwait(&dummy_throttle_cond, ltable->header_mut, &sleep_until);
          sleeping[s->merge_level] = false;
          pthread_cond_broadcast(&throttle_wokeup_cond);
          gettimeofday(&now, 0);
          if(s->merge_level == 0) { update_progress(c1, 0); }
          if(s->merge_level == 1) { update_progress(c2, 0); }
        } else {
          if(overshoot > 0) { s->need_tick = true; }
          break;
        }
      } while(1);
      rwlc_unlock(ltable->header_mut);
    } else {
      if(s->print_skipped == PRINT_SKIP) {
        if(!force) rwlc_writelock(ltable->header_mut);
        pretty_print(stdout);
        if(!force) rwlc_unlock(ltable->header_mut);
        s->print_skipped = 0;
      } else {
        s->print_skipped++;
      }
    }
  }
}

void mergeManager::read_tuple_from_small_component(int merge_level, datatuple * tup) {
  if(tup) {
    mergeStats * s = get_merge_stats(merge_level);
    (s->num_tuples_in_small)++;
    (s->bytes_in_small_delta) += tup->byte_length();
    (s->bytes_in_small) += tup->byte_length();
    update_progress(s, tup->byte_length());
    tick(s, true);
  }
}
void mergeManager::read_tuple_from_large_component(int merge_level, datatuple * tup) {
  if(tup) {
    mergeStats * s = get_merge_stats(merge_level);
    s->num_tuples_in_large++;
    s->bytes_in_large += tup->byte_length();
//    tick(s, false); // would be no-op; we just reduced current_size.
    update_progress(s, tup->byte_length());
    tick(s,false);
  }
}

void mergeManager::wrote_tuple(int merge_level, datatuple * tup) {
  mergeStats * s = get_merge_stats(merge_level);
  (s->num_tuples_out)++;
  (s->bytes_out) += tup->byte_length();

  // XXX this just updates stat's current size, and (perhaps) does a pretty print.  It should not need a mutex.
  //  update_progress(s, tup->byte_length());
  //  tick(s, false);
}

void mergeManager::finished_merge(int merge_level) {
  update_progress(get_merge_stats(merge_level), 0);
  tick(get_merge_stats(merge_level), false, true);  // XXX what does this do???
  get_merge_stats(merge_level)->active = false;
  if(merge_level != 0) {
    get_merge_stats(merge_level - 1)->mergeable_size = 0;
    update_progress(get_merge_stats(merge_level-1), 0);
  }
  gettimeofday(&get_merge_stats(merge_level)->done, 0);
  update_progress(get_merge_stats(merge_level), 0);
}

mergeManager::mergeManager(logtable<datatuple> *ltable):
  ltable(ltable),
  c0(new mergeStats(0, ltable ? ltable->max_c0_size : 10000000)),
  c1(new mergeStats(1, (int64_t)(ltable ? ((double)(ltable->max_c0_size) * *ltable->R()) : 100000000.0) )),
  c2(new mergeStats(2, 0)) {
  pthread_mutex_init(&throttle_mut, 0);
  pthread_mutex_init(&dummy_throttle_mut, 0);
  pthread_cond_init(&dummy_throttle_cond, 0);
  pthread_cond_init(&throttle_wokeup_cond, 0);
  struct timeval tv;
  gettimeofday(&tv, 0);
  sleeping[0] = false;
  sleeping[1] = false;
  sleeping[2] = false;
  double_to_ts(&c0->last_tick, tv_to_double(&tv));
  double_to_ts(&c1->last_tick, tv_to_double(&tv));
  double_to_ts(&c2->last_tick, tv_to_double(&tv));
}

void mergeManager::pretty_print(FILE * out) {
  pageid_t mb = 1024 * 1024;
  logtable<datatuple> * lt = (logtable<datatuple>*)ltable;
  bool have_c0  = false;
  bool have_c0m = false;
  bool have_c1  = false;
  bool have_c1m = false;
  bool have_c2  = false;
  if(lt) {
    have_c0  = NULL != lt->get_tree_c0();
    have_c0m = NULL != lt->get_tree_c0_mergeable();
    have_c1  = NULL != lt->get_tree_c1();
    have_c1m = NULL != lt->get_tree_c1_mergeable() ;
    have_c2  = NULL != lt->get_tree_c2();
  }

  double c0_out_progress = 100.0 * c0->current_size / c0->target_size;
  double c0_c1_in_progress = 100.0 * (c1->bytes_in_large + c1->bytes_in_small) / (c0->mergeable_size + c1->base_size);
  double c0_c1_out_progress = 100.0 * c1->current_size / c1->target_size;
  double c1_c2_progress = 100.0 * (c2->bytes_in_large + c2->bytes_in_small) / (c1->mergeable_size + c2->base_size);

  assert((!c1->active) || (c0_c1_in_progress >= -1 && c0_c1_in_progress < 102));
  assert((!c2->active) || (c1_c2_progress >= -1 && c1_c2_progress < 102));

  fprintf(out,"[merge progress MB/s window (lifetime)]: app [%s %6lldMB ~ %3.0f%% %6.1fsec %4.1f (%4.1f)] %s %s [%s %3.0f%% ~ %3.0f%% %4.1f (%4.1f)] %s %s [%s %3.0f%% %4.1f (%4.1f)] %s ",
      c0->active ? "RUN" : "---", (uint64_t)(c0->lifetime_consumed / mb), c0_out_progress, c0->lifetime_elapsed, c0->window_consumed/(((double)mb)*c0->window_elapsed), c0->lifetime_consumed/(((double)mb)*c0->lifetime_elapsed),
      have_c0 ? "C0" : "..",
      have_c0m ? "C0'" : "...",
      c1->active ? "RUN" : "---", c0_c1_in_progress, c0_c1_out_progress, c1->window_consumed/(((double)mb)*c1->window_elapsed), c1->lifetime_consumed/(((double)mb)*c1->lifetime_elapsed),
      have_c1 ? "C1" : "..",
      have_c1m ? "C1'" : "...",
      c2->active ? "RUN" : "---", c1_c2_progress, c2->window_consumed/(((double)mb)*c2->window_elapsed), c2->lifetime_consumed/(((double)mb)*c2->lifetime_elapsed),
      have_c2 ? "C2" : "..");
//#define PP_SIZES
#ifdef PP_SIZES
  fprintf(out, "[size in small/large, out, mergeable] C0 %4lld %4lld %4lld %4lld %4lld %4lld ",
          c0->target_size/mb, c0->current_size/mb, c0->bytes_in_small/mb,
          c0->bytes_in_large/mb, c0->bytes_out/mb, c0->mergeable_size/mb);

  fprintf(out, "C1 %4lld %4lld %4lld %4lld %4lld %4lld ",
          c1->target_size/mb, c1->current_size/mb, c1->bytes_in_small/mb,
          c1->bytes_in_large/mb, c1->bytes_out/mb, c1->mergeable_size/mb);

  fprintf(out, "C2 ---- %4lld %4lld %4lld %4lld %4lld ",
          /*----*/            c2->current_size/mb, c2->bytes_in_small/mb,
          c2->bytes_in_large/mb, c2->bytes_out/mb, c2->mergeable_size/mb);
#endif
//  fprintf(out, "Throttle: %6.1f%% (cur) %6.1f%% (overall) ", 100.0*(last_throttle_seconds/(last_elapsed_seconds)), 100.0*(throttle_seconds/(elapsed_seconds)));
//  fprintf(out, "C0 size %4lld resident %4lld ",
//               2*c0_queueSize/mb,
//               (c0->bytes_out - c0->bytes_in_large)/mb);
//  fprintf(out, "C1 size %4lld resident %4lld\r",
//               2*c1_queueSize/mb,
//               (c1->bytes_out - c1->bytes_in_large)/mb);
//  fprintf(out, "C2 size %4lld\r",
//               2*c2_queueSize/mb);
//  fprintf(out, "C1 MB/s (eff; active) %6.1f  C2 MB/s %6.1f\r",
//                ((double)c1_totalConsumed)/((double)c1_totalWorktime),
//                ((double)c2_totalConsumed)/((double)c2_totalWorktime));
  fflush(out);
  fprintf(out, "\r");
}
