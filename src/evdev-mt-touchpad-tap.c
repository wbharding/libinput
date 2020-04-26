/*
 * Copyright © 2013-2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "evdev-mt-touchpad.h"

#define DEFAULT_TAP_TIMEOUT_PERIOD ms2us(180)
#define DEFAULT_DRAG_TIMEOUT_PERIOD ms2us(300)
#define DEFAULT_TAP_MOVE_THRESHOLD 1.3 /* mm */

enum tap_event {
	TAP_EVENT_TOUCH = 12,
	TAP_EVENT_MOTION,
	TAP_EVENT_RELEASE,
	TAP_EVENT_BUTTON,
	TAP_EVENT_TIMEOUT,
	TAP_EVENT_THUMB,
	TAP_EVENT_PALM,
	TAP_EVENT_PALM_UP,
};

/*****************************************
 * DO NOT EDIT THIS FILE!
 *
 * Look at the state diagram in doc/touchpad-tap-state-machine.svg
 * (generated with https://draw.io)
 *
 * Any changes in this file must be represented in the diagram.
 */

static inline const char*
tap_state_to_str(enum tp_tap_state state)
{
	switch(state) {
	CASE_RETURN_STRING(TAP_STATE_IDLE);
	CASE_RETURN_STRING(TAP_STATE_HOLD);
	CASE_RETURN_STRING(TAP_STATE_TOUCH);
	CASE_RETURN_STRING(TAP_STATE_TAPPED);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_2);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_2_HOLD);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_2_RELEASE);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_3);
	CASE_RETURN_STRING(TAP_STATE_TOUCH_3_HOLD);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_WAIT);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_OR_DOUBLETAP);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_OR_TAP);
	CASE_RETURN_STRING(TAP_STATE_DRAGGING_2);
	CASE_RETURN_STRING(TAP_STATE_DEAD);
	}
	return NULL;
}

static inline const char*
tap_event_to_str(enum tap_event event)
{
	switch(event) {
	CASE_RETURN_STRING(TAP_EVENT_TOUCH);
	CASE_RETURN_STRING(TAP_EVENT_MOTION);
	CASE_RETURN_STRING(TAP_EVENT_RELEASE);
	CASE_RETURN_STRING(TAP_EVENT_TIMEOUT);
	CASE_RETURN_STRING(TAP_EVENT_BUTTON);
	CASE_RETURN_STRING(TAP_EVENT_THUMB);
	CASE_RETURN_STRING(TAP_EVENT_PALM);
	CASE_RETURN_STRING(TAP_EVENT_PALM_UP);
	}
	return NULL;
}

static inline void
log_tap_bug(struct tp_dispatch *tp, struct tp_touch *t, enum tap_event event)
{
	evdev_log_bug_libinput(tp->device,
			       "%d: invalid tap event %s in state %s\n",
			       t->index,
			       tap_event_to_str(event),
			       tap_state_to_str(tp->tap.state));

}

static void
tp_tap_notify(struct tp_dispatch *tp,
	      uint64_t time,
	      int nfingers,
	      enum libinput_button_state state)
{
	int32_t button;
	int32_t button_map[2][3] = {
		{ BTN_LEFT, BTN_RIGHT, BTN_MIDDLE },
		{ BTN_LEFT, BTN_MIDDLE, BTN_RIGHT },
	};

	assert(tp->tap.map < ARRAY_LENGTH(button_map));

	if (nfingers > 3)
		return;

	button = button_map[tp->tap.map][nfingers - 1];

	if (state == LIBINPUT_BUTTON_STATE_PRESSED)
		tp->tap.buttons_pressed |= (1 << nfingers);
	else
		tp->tap.buttons_pressed &= ~(1 << nfingers);

	evdev_pointer_notify_button(tp->device,
				    time,
				    button,
				    state);
}

static void
tp_tap_set_timer(struct tp_dispatch *tp, uint64_t time)
{
	libinput_timer_set(&tp->tap.timer, time + DEFAULT_TAP_TIMEOUT_PERIOD);
}

static void
tp_tap_set_drag_timer(struct tp_dispatch *tp, uint64_t time)
{
	libinput_timer_set(&tp->tap.timer, time + DEFAULT_DRAG_TIMEOUT_PERIOD);
}

static void
tp_tap_clear_timer(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->tap.timer);
}

static void
tp_tap_move_to_dead(struct tp_dispatch *tp, struct tp_touch *t)
{
	tp->tap.state = TAP_STATE_DEAD;
	t->tap.state = TAP_TOUCH_STATE_DEAD;
	tp_tap_clear_timer(tp);
}

static void
tp_tap_idle_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event, uint64_t time)
{
	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		break;
	case TAP_EVENT_MOTION:
		log_tap_bug(tp, t, event);
		break;
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		log_tap_bug(tp, t, event);
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch_handle_event(struct tp_dispatch *tp,
			  struct tp_touch *t,
			  enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_2;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp_tap_notify(tp,
			      tp->tap.saved_press_time,
			      1,
			      LIBINPUT_BUTTON_STATE_PRESSED);
		if (tp->tap.drag_enabled) {
			tp->tap.state = TAP_STATE_TAPPED;
			tp->tap.saved_release_time = time;
			tp_tap_set_timer(tp, time);
		} else {
			tp_tap_notify(tp,
				      time,
				      1,
				      LIBINPUT_BUTTON_STATE_RELEASED);
			tp->tap.state = TAP_STATE_IDLE;
		}
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_HOLD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		tp->tap.state = TAP_STATE_IDLE;
		t->tap.is_thumb = true;
		tp->tap.nfingers_down--;
		t->tap.state = TAP_TOUCH_STATE_DEAD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_hold_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_2;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		tp->tap.state = TAP_STATE_IDLE;
		t->tap.is_thumb = true;
		tp->tap.nfingers_down--;
		t->tap.state = TAP_TOUCH_STATE_DEAD;
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_tapped_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{
	switch (event) {
	case TAP_EVENT_MOTION:
	case TAP_EVENT_RELEASE:
		log_tap_bug(tp, t, event);
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_OR_DOUBLETAP;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
		log_tap_bug(tp, t, event);
		break;
	case TAP_EVENT_PALM:
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch2_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_3;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TOUCH_2_RELEASE;
		tp->tap.saved_release_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_TOUCH;
		tp_tap_set_timer(tp, time); /* overwrite timer */
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch2_hold_handle_event(struct tp_dispatch *tp,
				struct tp_touch *t,
				enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_3;
		tp->tap.saved_press_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_HOLD;
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_HOLD;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch2_release_handle_event(struct tp_dispatch *tp,
				   struct tp_touch *t,
				   enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		t->tap.state = TAP_TOUCH_STATE_DEAD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_RELEASE:
		tp_tap_notify(tp,
			      tp->tap.saved_press_time,
			      2,
			      LIBINPUT_BUTTON_STATE_PRESSED);
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      2,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_HOLD;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		/* There's only one saved press time and it's overwritten by
		 * the last touch down. So in the case of finger down, palm
		 * down, finger up, palm detected, we use the
		 * palm touch's press time here instead of the finger's press
		 * time. Let's wait and see if that's an issue.
		 */
		tp_tap_notify(tp,
			      tp->tap.saved_press_time,
			      1,
			      LIBINPUT_BUTTON_STATE_PRESSED);
		if (tp->tap.drag_enabled) {
			tp->tap.state = TAP_STATE_TAPPED;
			tp->tap.saved_release_time = time;
			tp_tap_set_timer(tp, time);
		} else {
			tp_tap_notify(tp,
				      time,
				      1,
				      LIBINPUT_BUTTON_STATE_RELEASED);
			tp->tap.state = TAP_STATE_IDLE;
		}
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch3_handle_event(struct tp_dispatch *tp,
			   struct tp_touch *t,
			   enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_TOUCH_3_HOLD;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		if (t->tap.state == TAP_TOUCH_STATE_TOUCH) {
			tp_tap_notify(tp,
				      tp->tap.saved_press_time,
				      3,
				      LIBINPUT_BUTTON_STATE_PRESSED);
			tp_tap_notify(tp, time, 3, LIBINPUT_BUTTON_STATE_RELEASED);
		}
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_TOUCH_2;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_touch3_hold_handle_event(struct tp_dispatch *tp,
				struct tp_touch *t,
				enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_MOTION:
		tp_tap_move_to_dead(tp, t);
		break;
	case TAP_EVENT_TIMEOUT:
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_TOUCH_2_HOLD;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dragging_or_doubletap_handle_event(struct tp_dispatch *tp,
					  struct tp_touch *t,
					  enum tap_event event, uint64_t time)
{
	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_TAPPED;
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		tp_tap_notify(tp,
			      tp->tap.saved_press_time,
			      1,
			      LIBINPUT_BUTTON_STATE_PRESSED);
		tp->tap.saved_release_time = time;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_DRAGGING;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_TAPPED;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dragging_handle_event(struct tp_dispatch *tp,
			     struct tp_touch *t,
			     enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		break;
	case TAP_EVENT_RELEASE:
		if (tp->tap.drag_lock_enabled) {
			tp->tap.state = TAP_STATE_DRAGGING_WAIT;
			tp_tap_set_drag_timer(tp, time);
		} else {
			tp_tap_notify(tp,
				      time,
				      1,
				      LIBINPUT_BUTTON_STATE_RELEASED);
			tp->tap.state = TAP_STATE_IDLE;
		}
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		/* noop */
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dragging_wait_handle_event(struct tp_dispatch *tp,
				  struct tp_touch *t,
				  enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_OR_TAP;
		tp_tap_set_timer(tp, time);
		break;
	case TAP_EVENT_RELEASE:
	case TAP_EVENT_MOTION:
		break;
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
	case TAP_EVENT_PALM:
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dragging_tap_handle_event(struct tp_dispatch *tp,
				  struct tp_touch *t,
				  enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DRAGGING_2;
		tp_tap_clear_timer(tp);
		break;
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_IDLE;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		tp->tap.state = TAP_STATE_DRAGGING;
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp_tap_notify(tp,
			      tp->tap.saved_release_time,
			      1,
			      LIBINPUT_BUTTON_STATE_RELEASED);
		tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dragging2_handle_event(struct tp_dispatch *tp,
			      struct tp_touch *t,
			      enum tap_event event, uint64_t time)
{

	switch (event) {
	case TAP_EVENT_RELEASE:
		tp->tap.state = TAP_STATE_DRAGGING;
		break;
	case TAP_EVENT_TOUCH:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
		/* noop */
		break;
	case TAP_EVENT_BUTTON:
		tp->tap.state = TAP_STATE_DEAD;
		tp_tap_notify(tp, time, 1, LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
		tp->tap.state = TAP_STATE_DRAGGING_OR_DOUBLETAP;
		break;
	case TAP_EVENT_PALM_UP:
		break;
	}
}

static void
tp_tap_dead_handle_event(struct tp_dispatch *tp,
			 struct tp_touch *t,
			 enum tap_event event,
			 uint64_t time)
{

	switch (event) {
	case TAP_EVENT_RELEASE:
		if (tp->tap.nfingers_down == 0)
			tp->tap.state = TAP_STATE_IDLE;
		break;
	case TAP_EVENT_TOUCH:
	case TAP_EVENT_MOTION:
	case TAP_EVENT_TIMEOUT:
	case TAP_EVENT_BUTTON:
		break;
	case TAP_EVENT_THUMB:
		break;
	case TAP_EVENT_PALM:
	case TAP_EVENT_PALM_UP:
		if (tp->tap.nfingers_down == 0)
			tp->tap.state = TAP_STATE_IDLE;
		break;
	}
}

static void
tp_tap_handle_event(struct tp_dispatch *tp,
		    struct tp_touch *t,
		    enum tap_event event,
		    uint64_t time)
{
	enum tp_tap_state current;

	current = tp->tap.state;

	switch(tp->tap.state) {
	case TAP_STATE_IDLE:
		tp_tap_idle_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH:
		tp_tap_touch_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_HOLD:
		tp_tap_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TAPPED:
		tp_tap_tapped_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_2:
		tp_tap_touch2_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_2_HOLD:
		tp_tap_touch2_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_2_RELEASE:
		tp_tap_touch2_release_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_3:
		tp_tap_touch3_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_TOUCH_3_HOLD:
		tp_tap_touch3_hold_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_OR_DOUBLETAP:
		tp_tap_dragging_or_doubletap_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING:
		tp_tap_dragging_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_WAIT:
		tp_tap_dragging_wait_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_OR_TAP:
		tp_tap_dragging_tap_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DRAGGING_2:
		tp_tap_dragging2_handle_event(tp, t, event, time);
		break;
	case TAP_STATE_DEAD:
		tp_tap_dead_handle_event(tp, t, event, time);
		break;
	}

	if (tp->tap.state == TAP_STATE_IDLE || tp->tap.state == TAP_STATE_DEAD)
		tp_tap_clear_timer(tp);

	if (current != tp->tap.state)
		evdev_log_debug(tp->device,
			  "tap: touch %d state %s → %s → %s\n",
			  t ? (int)t->index : -1,
			  tap_state_to_str(current),
			  tap_event_to_str(event),
			  tap_state_to_str(tp->tap.state));
}

static bool
tp_tap_exceeds_motion_threshold(struct tp_dispatch *tp,
				struct tp_touch *t)
{
	struct phys_coords mm =
		tp_phys_delta(tp, device_delta(t->point, t->tap.initial));

	/* if we have more fingers down than slots, we know that synaptics
	 * touchpads are likely to give us pointer jumps.
	 * This triggers the movement threshold, making three-finger taps
	 * less reliable (#101435)
	 *
	 * This uses the real nfingers_down, not the one for taps.
	 */
	if (tp->device->model_flags & EVDEV_MODEL_SYNAPTICS_SERIAL_TOUCHPAD &&
	    (tp->nfingers_down > 2 || tp->old_nfingers_down > 2) &&
	    (tp->nfingers_down > tp->num_slots ||
	     tp->old_nfingers_down > tp->num_slots)) {
		return false;
	}

	/* Semi-mt devices will give us large movements on finger release,
	 * depending which touch is released. Make sure we ignore any
	 * movement in the same frame as a finger change.
	 */
	if (tp->semi_mt && tp->nfingers_down != tp->old_nfingers_down)
		return false;

	return length_in_mm(mm) > DEFAULT_TAP_MOVE_THRESHOLD;
}

static bool
tp_tap_enabled(struct tp_dispatch *tp)
{
	return tp->tap.enabled && !tp->tap.suspended;
}

int
tp_tap_handle_state(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	int filter_motion = 0;

	if (!tp_tap_enabled(tp))
		return 0;

	/* Handle queued button pressed events from clickpads. For touchpads
	 * with separate physical buttons, ignore button pressed events so they
	 * don't interfere with tapping. */
	if (tp->buttons.is_clickpad && tp->queued & TOUCHPAD_EVENT_BUTTON_PRESS)
		tp_tap_handle_event(tp, NULL, TAP_EVENT_BUTTON, time);

	tp_for_each_touch(tp, t) {
		if (!t->dirty || t->state == TOUCH_NONE)
			continue;

		if (tp->buttons.is_clickpad &&
		    tp->queued & TOUCHPAD_EVENT_BUTTON_PRESS)
			t->tap.state = TAP_TOUCH_STATE_DEAD;

		/* If a touch was considered thumb for tapping once, we
		 * ignore it for the rest of lifetime */
		if (t->tap.is_thumb)
			continue;

		/* A palm tap needs to be properly relased because we might
		 * be who-knows-where in the state machine. Otherwise, we
		 * ignore any event from it.
		 */
		if (t->tap.is_palm) {
			if (t->state == TOUCH_END)
				tp_tap_handle_event(tp,
						    t,
						    TAP_EVENT_PALM_UP,
						    time);
			continue;
		}

		if (t->state == TOUCH_HOVERING)
			continue;

		if (t->palm.state != PALM_NONE) {
			assert(!t->tap.is_palm);
			tp_tap_handle_event(tp, t, TAP_EVENT_PALM, time);
			t->tap.is_palm = true;
			t->tap.state = TAP_TOUCH_STATE_DEAD;
			if (t->state != TOUCH_BEGIN) {
				assert(tp->tap.nfingers_down > 0);
				tp->tap.nfingers_down--;
			}
		} else if (t->state == TOUCH_BEGIN) {
			/* The simple version: if a touch is a thumb on
			 * begin we ignore it. All other thumb touches
			 * follow the normal tap state for now */
			if (tp_thumb_ignored_for_tap(tp, t)) {
				t->tap.is_thumb = true;
				continue;
			}

			t->tap.state = TAP_TOUCH_STATE_TOUCH;
			t->tap.initial = t->point;
			tp->tap.nfingers_down++;
			tp_tap_handle_event(tp, t, TAP_EVENT_TOUCH, time);

			/* If we think this is a palm, pretend there's a
			 * motion event which will prevent tap clicks
			 * without requiring extra states in the FSM.
			 */
			if (tp_palm_tap_is_palm(tp, t))
				tp_tap_handle_event(tp, t, TAP_EVENT_MOTION, time);

		} else if (t->state == TOUCH_END) {
			if (t->was_down) {
				assert(tp->tap.nfingers_down >= 1);
				tp->tap.nfingers_down--;
				tp_tap_handle_event(tp, t, TAP_EVENT_RELEASE, time);
			}
			t->tap.state = TAP_TOUCH_STATE_IDLE;
		} else if (tp->tap.state != TAP_STATE_IDLE &&
			   tp_thumb_ignored(tp, t)) {
			tp_tap_handle_event(tp, t, TAP_EVENT_THUMB, time);
		} else if (tp->tap.state != TAP_STATE_IDLE &&
			   tp_tap_exceeds_motion_threshold(tp, t)) {
			struct tp_touch *tmp;

			/* Any touch exceeding the threshold turns all
			 * touches into DEAD */
			tp_for_each_touch(tp, tmp) {
				if (tmp->tap.state == TAP_TOUCH_STATE_TOUCH)
					tmp->tap.state = TAP_TOUCH_STATE_DEAD;
			}

			tp_tap_handle_event(tp, t, TAP_EVENT_MOTION, time);
		}
	}

	/**
	 * In any state where motion exceeding the move threshold would
	 * move to the next state, filter that motion until we actually
	 * exceed it. This prevents small motion events while we're waiting
	 * on a decision if a tap is a tap.
	 */
	switch (tp->tap.state) {
	case TAP_STATE_TOUCH:
	case TAP_STATE_TAPPED:
	case TAP_STATE_DRAGGING_OR_DOUBLETAP:
	case TAP_STATE_DRAGGING_OR_TAP:
	case TAP_STATE_TOUCH_2:
	case TAP_STATE_TOUCH_3:
		filter_motion = 1;
		break;

	default:
		break;

	}

	assert(tp->tap.nfingers_down <= tp->nfingers_down);
	if (tp->nfingers_down == 0)
		assert(tp->tap.nfingers_down == 0);

	return filter_motion;
}

static inline void
tp_tap_update_map(struct tp_dispatch *tp)
{
	if (tp->tap.state != TAP_STATE_IDLE)
		return;

	if (tp->tap.map != tp->tap.want_map)
		tp->tap.map = tp->tap.want_map;
}

void
tp_tap_post_process_state(struct tp_dispatch *tp)
{
	tp_tap_update_map(tp);
}

static void
tp_tap_handle_timeout(uint64_t time, void *data)
{
	struct tp_dispatch *tp = data;
	struct tp_touch *t;

	tp_tap_handle_event(tp, NULL, TAP_EVENT_TIMEOUT, time);

	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE ||
		    t->tap.state == TAP_TOUCH_STATE_IDLE)
			continue;

		t->tap.state = TAP_TOUCH_STATE_DEAD;
	}
}

static void
tp_tap_enabled_update(struct tp_dispatch *tp, bool suspended, bool enabled, uint64_t time)
{
	bool was_enabled = tp_tap_enabled(tp);

	tp->tap.suspended = suspended;
	tp->tap.enabled = enabled;

	if (tp_tap_enabled(tp) == was_enabled)
		return;

	if (tp_tap_enabled(tp)) {
		struct tp_touch *t;

		/* On resume, all touches are considered palms */
		tp_for_each_touch(tp, t) {
			if (t->state == TOUCH_NONE)
				continue;

			t->tap.is_palm = true;
			t->tap.state = TAP_TOUCH_STATE_DEAD;
		}

		tp->tap.state = TAP_STATE_IDLE;
		tp->tap.nfingers_down = 0;
	} else {
		tp_release_all_taps(tp, time);
	}
}

static int
tp_tap_config_count(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	return min(tp->ntouches, 3U); /* we only do up to 3 finger tap */
}

static enum libinput_config_status
tp_tap_config_set_enabled(struct libinput_device *device,
			  enum libinput_config_tap_state enabled)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	tp_tap_enabled_update(tp, tp->tap.suspended,
			      (enabled == LIBINPUT_CONFIG_TAP_ENABLED),
			      libinput_now(device->seat->libinput));

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_tap_state
tp_tap_config_is_enabled(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	return tp->tap.enabled ? LIBINPUT_CONFIG_TAP_ENABLED :
				 LIBINPUT_CONFIG_TAP_DISABLED;
}

static enum libinput_config_tap_state
tp_tap_default(struct evdev_device *evdev)
{
	/**
	 * If we don't have a left button we must have tapping enabled by
	 * default.
	 */
	if (!libevdev_has_event_code(evdev->evdev, EV_KEY, BTN_LEFT))
		return LIBINPUT_CONFIG_TAP_ENABLED;

	/**
	 * Tapping is disabled by default for two reasons:
	 * * if you don't know that tapping is a thing (or enabled by
	 *   default), you get spurious mouse events that make the desktop
	 *   feel buggy.
	 * * if you do know what tapping is and you want it, you
	 *   usually know where to enable it, or at least you can search for
	 *   it.
	 */
	return LIBINPUT_CONFIG_TAP_DISABLED;
}

static enum libinput_config_tap_state
tp_tap_config_get_default(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);

	return tp_tap_default(evdev);
}

static enum libinput_config_status
tp_tap_config_set_map(struct libinput_device *device,
		      enum libinput_config_tap_button_map map)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	tp->tap.want_map = map;

	tp_tap_update_map(tp);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_tap_button_map
tp_tap_config_get_map(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	return tp->tap.want_map;
}

static enum libinput_config_tap_button_map
tp_tap_config_get_default_map(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_TAP_MAP_LRM;
}

static enum libinput_config_status
tp_tap_config_set_drag_enabled(struct libinput_device *device,
			       enum libinput_config_drag_state enabled)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	tp->tap.drag_enabled = enabled;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_drag_state
tp_tap_config_get_drag_enabled(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	return tp->tap.drag_enabled;
}

static inline enum libinput_config_drag_state
tp_drag_default(struct evdev_device *device)
{
	return LIBINPUT_CONFIG_DRAG_ENABLED;
}

static enum libinput_config_drag_state
tp_tap_config_get_default_drag_enabled(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);

	return tp_drag_default(evdev);
}

static enum libinput_config_status
tp_tap_config_set_draglock_enabled(struct libinput_device *device,
				   enum libinput_config_drag_lock_state enabled)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	tp->tap.drag_lock_enabled = enabled;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_drag_lock_state
tp_tap_config_get_draglock_enabled(struct libinput_device *device)
{
	struct evdev_dispatch *dispatch = evdev_device(device)->dispatch;
	struct tp_dispatch *tp = tp_dispatch(dispatch);

	return tp->tap.drag_lock_enabled;
}

static inline enum libinput_config_drag_lock_state
tp_drag_lock_default(struct evdev_device *device)
{
	return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
}

static enum libinput_config_drag_lock_state
tp_tap_config_get_default_draglock_enabled(struct libinput_device *device)
{
	struct evdev_device *evdev = evdev_device(device);

	return tp_drag_lock_default(evdev);
}

void
tp_init_tap(struct tp_dispatch *tp)
{
	char timer_name[64];

	tp->tap.config.count = tp_tap_config_count;
	tp->tap.config.set_enabled = tp_tap_config_set_enabled;
	tp->tap.config.get_enabled = tp_tap_config_is_enabled;
	tp->tap.config.get_default = tp_tap_config_get_default;
	tp->tap.config.set_map = tp_tap_config_set_map;
	tp->tap.config.get_map = tp_tap_config_get_map;
	tp->tap.config.get_default_map = tp_tap_config_get_default_map;
	tp->tap.config.set_drag_enabled = tp_tap_config_set_drag_enabled;
	tp->tap.config.get_drag_enabled = tp_tap_config_get_drag_enabled;
	tp->tap.config.get_default_drag_enabled = tp_tap_config_get_default_drag_enabled;
	tp->tap.config.set_draglock_enabled = tp_tap_config_set_draglock_enabled;
	tp->tap.config.get_draglock_enabled = tp_tap_config_get_draglock_enabled;
	tp->tap.config.get_default_draglock_enabled = tp_tap_config_get_default_draglock_enabled;
	tp->device->base.config.tap = &tp->tap.config;

	tp->tap.state = TAP_STATE_IDLE;
	tp->tap.enabled = tp_tap_default(tp->device);
	tp->tap.map = LIBINPUT_CONFIG_TAP_MAP_LRM;
	tp->tap.want_map = tp->tap.map;
	tp->tap.drag_enabled = tp_drag_default(tp->device);
	tp->tap.drag_lock_enabled = tp_drag_lock_default(tp->device);

	snprintf(timer_name,
		 sizeof(timer_name),
		 "%s tap",
		 evdev_device_get_sysname(tp->device));
	libinput_timer_init(&tp->tap.timer,
			    tp_libinput_context(tp),
			    timer_name,
			    tp_tap_handle_timeout, tp);
}

void
tp_remove_tap(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->tap.timer);
}

void
tp_release_all_taps(struct tp_dispatch *tp, uint64_t now)
{
	struct tp_touch *t;
	int i;

	for (i = 1; i <= 3; i++) {
		if (tp->tap.buttons_pressed & (1 << i))
			tp_tap_notify(tp, now, i, LIBINPUT_BUTTON_STATE_RELEASED);
	}

	/* To neutralize all current touches, we make them all palms */
	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE)
			continue;

		if (t->tap.is_palm)
			continue;

		t->tap.is_palm = true;
		t->tap.state = TAP_TOUCH_STATE_DEAD;
	}

	tp->tap.state = TAP_STATE_IDLE;
	tp->tap.nfingers_down = 0;
}

void
tp_tap_suspend(struct tp_dispatch *tp, uint64_t time)
{
	tp_tap_enabled_update(tp, true, tp->tap.enabled, time);
}

void
tp_tap_resume(struct tp_dispatch *tp, uint64_t time)
{
	tp_tap_enabled_update(tp, false, tp->tap.enabled, time);
}

bool
tp_tap_dragging(const struct tp_dispatch *tp)
{
	switch (tp->tap.state) {
	case TAP_STATE_DRAGGING:
	case TAP_STATE_DRAGGING_2:
	case TAP_STATE_DRAGGING_WAIT:
	case TAP_STATE_DRAGGING_OR_TAP:
		return true;
	default:
		return false;
	}
}
