/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Rain Graph watchface.
 *
 * Shows the current time plus an adaptive line graph of upcoming rain
 * intensity. The phone (see src/js/pebble-js-app.js) fetches the KNMI
 * precipitation forecast, picks a time window that stretches from the coming
 * hour (rain imminent) up to the rest of the day (rain far off), samples it
 * into a normalised series and sends it here for drawing.
 */

#include <pebble.h>

// Keys must match appinfo.json "appKeys".
enum {
  KEY_REQUEST = 0,
  KEY_SPAN_MIN = 1,
  KEY_PEAK_MMH = 2,
  KEY_RAIN_AT_MIN = 3,
  KEY_NUM_POINTS = 4,
  KEY_POINTS = 5,
  KEY_STATUS = 6,
};

#define MAX_POINTS 64
#define REFRESH_MINUTES 10

// Persist slots so the face is not blank right after launch.
enum {
  PERSIST_POINTS = 1,
  PERSIST_NUM_POINTS = 2,
  PERSIST_SPAN_MIN = 3,
  PERSIST_PEAK_MMH = 4,
  PERSIST_RAIN_AT_MIN = 5,
};

static Window *s_window;
static TextLayer *s_time_layer;
static TextLayer *s_subtitle_layer;
static Layer *s_graph_layer;

static char s_time_buffer[8];
static char s_subtitle_buffer[40];

// Forecast state.
static uint8_t s_points[MAX_POINTS];
static int s_num_points = 0;
static int s_span_min = 60;
static int s_peak_mmh = 0;       // hundredths of mm/h
static int s_rain_at_min = -1;   // minutes until first rain, -1 == none
static char s_status[24] = "";

static void prv_format_clock(int hour, int minute, char *out, size_t out_len) {
  if (clock_is_24h_style()) {
    snprintf(out, out_len, "%02d:%02d", hour, minute);
  } else {
    int h12 = hour % 12;
    if (h12 == 0) {
      h12 = 12;
    }
    snprintf(out, out_len, "%d:%02d", h12, minute);
  }
}

// Build the subtitle from the current forecast state.
static void prv_update_subtitle(void) {
  if (s_num_points < 2) {
    snprintf(s_subtitle_buffer, sizeof(s_subtitle_buffer), "%s",
             s_status[0] ? s_status : "Loading...");
  } else if (s_rain_at_min < 0) {
    snprintf(s_subtitle_buffer, sizeof(s_subtitle_buffer), "No rain expected");
  } else {
    time_t onset_t = time(NULL) + s_rain_at_min * 60;
    struct tm *onset = localtime(&onset_t);
    char onset_buf[8];
    prv_format_clock(onset->tm_hour, onset->tm_min, onset_buf, sizeof(onset_buf));
    int whole = s_peak_mmh / 100;
    int tenths = (s_peak_mmh % 100) / 10;
    snprintf(s_subtitle_buffer, sizeof(s_subtitle_buffer), "Rain %s  %d.%d mm/h",
             onset_buf, whole, tenths);
  }
  text_layer_set_text(s_subtitle_layer, s_subtitle_buffer);
}

static void prv_draw_x_label(GContext *ctx, GRect bounds, int x, int minutes_ahead,
                             GTextAlignment align) {
  time_t t = time(NULL) + minutes_ahead * 60;
  struct tm *lt = localtime(&t);
  char buf[8];
  prv_format_clock(lt->tm_hour, lt->tm_min, buf, sizeof(buf));

  GRect r;
  if (align == GTextAlignmentRight) {
    r = GRect(x - 44, bounds.size.h - 15, 44, 14);
  } else {
    r = GRect(x, bounds.size.h - 15, 44, 14);
  }
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14), r,
                     GTextOverflowModeFill, align, NULL);
}

static void prv_graph_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Plot area leaves room for x-axis labels at the bottom.
  const int label_h = 16;
  GRect plot = GRect(2, 2, bounds.size.w - 4, bounds.size.h - 2 - label_h);
  int base_y = plot.origin.y + plot.size.h;

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_text_color(ctx, GColorBlack);

  // Baseline (the ground / x-axis).
  graphics_draw_line(ctx, GPoint(plot.origin.x, base_y),
                     GPoint(plot.origin.x + plot.size.w, base_y));

  if (s_num_points < 2) {
    graphics_draw_text(ctx, s_subtitle_buffer,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18), plot,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  // Map sample index/value to screen coordinates.
  GPoint prev = GPoint(0, 0);
  for (int i = 0; i < s_num_points; i++) {
    int x = plot.origin.x + (plot.size.w * i) / (s_num_points - 1);
    int y = base_y - (plot.size.h * s_points[i]) / 255;
    GPoint cur = GPoint(x, y);

    // Fill the area under the curve so showers read as solid bars of rain.
#if defined(PBL_COLOR)
    graphics_context_set_stroke_color(ctx, GColorVividCerulean);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    if (s_points[i] > 0) {
      graphics_draw_line(ctx, GPoint(x, base_y - 1), GPoint(x, y));
    }

    if (i > 0) {
#if defined(PBL_COLOR)
      graphics_context_set_stroke_color(ctx, GColorBlue);
#else
      graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
      graphics_draw_line(ctx, prev, cur);
    }
    prev = cur;
  }

  // Marker for when the rain starts.
  if (s_rain_at_min >= 0 && s_rain_at_min <= s_span_min) {
    int mx = plot.origin.x + (plot.size.w * s_rain_at_min) / s_span_min;
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    for (int y = plot.origin.y; y < base_y; y += 4) {
      graphics_draw_pixel(ctx, GPoint(mx, y));
    }
  }

  // X-axis labels: "now" on the left, the window end on the right.
  graphics_context_set_text_color(ctx, GColorBlack);
  prv_draw_x_label(ctx, bounds, plot.origin.x, 0, GTextAlignmentLeft);
  prv_draw_x_label(ctx, bounds, plot.origin.x + plot.size.w, s_span_min,
                   GTextAlignmentRight);
}

static void prv_update_time(void) {
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  prv_format_clock(lt->tm_hour, lt->tm_min, s_time_buffer, sizeof(s_time_buffer));
  text_layer_set_text(s_time_layer, s_time_buffer);
}

static void prv_request_update(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, KEY_REQUEST, 1);
  dict_write_end(iter);
  app_message_outbox_send();
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_time();
  if (tick_time->tm_min % REFRESH_MINUTES == 0) {
    prv_request_update();
  }
}

static void prv_save_state(void) {
  persist_write_data(PERSIST_POINTS, s_points, s_num_points);
  persist_write_int(PERSIST_NUM_POINTS, s_num_points);
  persist_write_int(PERSIST_SPAN_MIN, s_span_min);
  persist_write_int(PERSIST_PEAK_MMH, s_peak_mmh);
  persist_write_int(PERSIST_RAIN_AT_MIN, s_rain_at_min);
}

static void prv_load_state(void) {
  if (persist_exists(PERSIST_NUM_POINTS)) {
    s_num_points = persist_read_int(PERSIST_NUM_POINTS);
    if (s_num_points > MAX_POINTS) {
      s_num_points = MAX_POINTS;
    }
    persist_read_data(PERSIST_POINTS, s_points, s_num_points);
    s_span_min = persist_read_int(PERSIST_SPAN_MIN);
    s_peak_mmh = persist_read_int(PERSIST_PEAK_MMH);
    s_rain_at_min = persist_read_int(PERSIST_RAIN_AT_MIN);
  }
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  if ((t = dict_find(iter, KEY_STATUS))) {
    strncpy(s_status, t->value->cstring, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
  }
  if ((t = dict_find(iter, KEY_SPAN_MIN))) {
    s_span_min = t->value->int32;
  }
  if ((t = dict_find(iter, KEY_PEAK_MMH))) {
    s_peak_mmh = t->value->int32;
  }
  if ((t = dict_find(iter, KEY_RAIN_AT_MIN))) {
    s_rain_at_min = t->value->int32;
  }
  if ((t = dict_find(iter, KEY_POINTS))) {
    int n = t->length;
    if (n > MAX_POINTS) {
      n = MAX_POINTS;
    }
    memcpy(s_points, t->value->data, n);
    s_num_points = n;
    s_status[0] = '\0';
    prv_save_state();
  }

  prv_update_subtitle();
  layer_mark_dirty(s_graph_layer);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_time_layer = text_layer_create(GRect(0, 2, bounds.size.w, 34));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_subtitle_layer = text_layer_create(GRect(0, 36, bounds.size.w, 18));
  text_layer_set_text_alignment(s_subtitle_layer, GTextAlignmentCenter);
  text_layer_set_font(s_subtitle_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(root, text_layer_get_layer(s_subtitle_layer));

  s_graph_layer = layer_create(GRect(0, 56, bounds.size.w, bounds.size.h - 56));
  layer_set_update_proc(s_graph_layer, prv_graph_update_proc);
  layer_add_child(root, s_graph_layer);

  prv_update_time();
  prv_update_subtitle();
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_subtitle_layer);
  layer_destroy(s_graph_layer);
}

static void prv_init(void) {
  prv_load_state();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(prv_inbox_received);
  app_message_open(512, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);

  prv_request_update();
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
