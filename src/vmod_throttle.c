#include <stdlib.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"

#include "vcc_if.h"

//Individual call
struct vmodth_call {
  double time;
  struct vmodth_call *next;
  struct vmodth_call *prev;
};

//Call window
struct vmodth_call_win {
  //Def
  int length;
  int max_calls;
  //Status
  int nb_calls;
  struct vmodth_call *last_call;
};

//Call set
struct vmodth_calls {
  struct vmodth_call* first;
  struct vmodth_call* last;
  struct vmodth_call_win* wins;
  int nb_wins;
};

int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
	return (0);
}


// Private: Update the window counter (e.g. the last minute calls counter)
void _vmod_update_window_counter(struct vmodth_call_win* call_win, double now) {
  while(call_win->last_call && call_win->last_call->time < now - call_win->length) {
      call_win->last_call = call_win->last_call->prev;
      call_win->nb_calls--;
  }
}

// Private: Return the amount of time to wait to be allowed in this time window
double _vmod_get_time_wait_for_window_compliance(struct vmodth_call_win* call_win, double now) {
  double result = 0.0;

  if(call_win->nb_calls >= call_win->max_calls) {
    result = call_win->length - (now - call_win->last_call->time);
  }

  return result;
}

// Private: Update the window counter (e.g. the last minute calls counter)
void _vmod_increment_window_counter(struct vmodth_call* new_call, struct vmodth_call_win* call_win) {
  call_win->nb_calls++;
  if(call_win->last_call == NULL) {
    call_win->last_call = new_call;
  }  
}

// Private: Remove older entries
void _vmod_remove_older_entries(struct vmodth_calls* calls, int max_window_length) {
  struct vmodth_call *prev;

  while(calls->last->prev) {
    if(calls->last->time < calls->first->time - max_window_length) {
      prev = calls->last->prev;
      free(calls->last);
      calls->last = prev;
      prev->next = NULL;
    }
    else {
      break;
    }
  }
}

// Public: is_allowed VCL command
double
vmod_is_allowed(struct sess *sp, struct vmod_priv *pc, const char* calls_throttle_key, int call_limit_per_sec, int call_limit_per_min, int call_limit_per_hour)
{
  struct vmodth_calls *calls;
	double result = 0;

  //Init data structure
  calls = ((struct vmodth_calls*)pc->priv);
  if (!calls) {
    pc->priv = malloc(sizeof(struct vmodth_calls));
    AN(pc->priv);
    pc->free = free;
    calls = ((struct vmodth_calls*)pc->priv);
    calls->first = NULL;
    calls->last = NULL;
    calls->nb_wins = 3;
    calls->wins = malloc(sizeof(struct vmodth_call_win)*calls->nb_wins);
    for(int i = 0; i < calls->nb_wins; i++) {
      calls->wins[i].nb_calls = 0;
      calls->wins[i].last_call = NULL;
    }
    calls->wins[0].length = 1;
    calls->wins[0].max_calls = call_limit_per_sec;
    calls->wins[1].length = 60;
    calls->wins[1].max_calls = call_limit_per_min;
    calls->wins[2].length = 3600;
    calls->wins[2].max_calls = call_limit_per_hour;
  }

  //Get time
  //TODO: first a faster one, let's avoid a syscall
  struct timeval tv;
  gettimeofday(&tv, NULL);
  double now = tv.tv_sec+tv.tv_usec/1000000.0;

  //Update the windows counters and pointers, with the current time
  for(int i = 0; i < calls->nb_wins; i++) {
    _vmod_update_window_counter(&calls->wins[i], now);
  }

  //Now lets check if we are allowed
  //If we are not, let's return how much time we should wait before being allowed
  double win_wait;
  for(int i = 0; i < calls->nb_wins; i++) {
    win_wait = _vmod_get_time_wait_for_window_compliance(&calls->wins[i], now);
    if(win_wait > result) {
      result = win_wait;
    }
  }
  if(result > 0.0) {
    return result;
  }

  //We are authorized
  //Add the call in the call list
  struct vmodth_call* new_call = malloc(sizeof(struct vmodth_call));
  new_call->time = now;
  new_call->next = calls->first;
  new_call->prev = NULL;
  if(calls->first) {
    calls->first->prev = new_call;
  }
  calls->first = new_call;
  if(calls->last == NULL) {
    calls->last = new_call;
  }

  //Increment the windows counters and update if necessary their pointers
  for(int i = 0; i < calls->nb_wins; i++) {
    _vmod_increment_window_counter(new_call, &calls->wins[i]);
  }

  //Remove the older entries older than the maximum window we are tracking, 1h
  _vmod_remove_older_entries(calls, 3600);

  return result;
}