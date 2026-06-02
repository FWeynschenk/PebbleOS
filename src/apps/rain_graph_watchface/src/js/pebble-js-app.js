/*
 * Rain Graph watchface - phone side.
 *
 * Fetches precipitation forecast from the Open-Meteo API using the KNMI
 * HARMONIE model (models=knmi_seamless) - the same KNMI forecast data behind
 * the KNMI weather app - and pushes an adaptive rain-intensity line to the
 * watch.
 *
 * Open-Meteo is keyless, returns JSON and is CORS friendly, which makes it a
 * good fit for a PebbleKit JS app. minutely_15 gives 15-minute resolution for
 * the whole forecast day, which is what the adaptive window samples from.
 */

// Number of samples drawn on the watch. Keep small: this is a byte array sent
// over AppMessage.
var NUM_POINTS = 48;

// Below this intensity (mm/h) we consider it "not raining".
var RAIN_THRESHOLD_MMH = 0.1;

// Default location (De Bilt / KNMI HQ) used when geolocation is unavailable.
var DEFAULT_LAT = 52.10;
var DEFAULT_LON = 5.18;

function log(msg) {
  console.log("[rain] " + msg);
}

function sendStatus(text) {
  Pebble.sendAppMessage({ "STATUS": text });
}

// Parse an Open-Meteo local ISO timestamp ("2026-06-02T14:30") into epoch ms,
// treating it as local time (no timezone suffix is returned with timezone=auto).
function parseLocalIso(s) {
  var m = s.match(/(\d+)-(\d+)-(\d+)T(\d+):(\d+)/);
  if (!m) {
    return NaN;
  }
  return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], 0, 0).getTime();
}

// Build a [{t: epochMs, mmh: intensity}] timeline from the minutely_15 block.
// minutely_15 precipitation is mm accumulated over 15 minutes, so *4 -> mm/h.
function buildTimeline(data) {
  var series = [];
  var block = data.minutely_15;
  var scale = 4.0;
  if (!block || !block.time || !block.precipitation) {
    // Fall back to hourly (already mm/h-ish, mm over 1 hour).
    block = data.hourly;
    scale = 1.0;
  }
  if (!block || !block.time) {
    return series;
  }
  for (var i = 0; i < block.time.length; i++) {
    var p = block.precipitation[i];
    series.push({
      t: parseLocalIso(block.time[i]),
      mmh: (p === null || p === undefined || isNaN(p)) ? 0 : p * scale
    });
  }
  return series;
}

// Linear interpolation of intensity at an arbitrary epoch ms.
function intensityAt(series, t) {
  if (series.length === 0) {
    return 0;
  }
  if (t <= series[0].t) {
    return series[0].mmh;
  }
  if (t >= series[series.length - 1].t) {
    return series[series.length - 1].mmh;
  }
  for (var i = 1; i < series.length; i++) {
    if (series[i].t >= t) {
      var a = series[i - 1];
      var b = series[i];
      var f = (t - a.t) / (b.t - a.t);
      return a.mmh + (b.mmh - a.mmh) * f;
    }
  }
  return series[series.length - 1].mmh;
}

// Decide how far into the future the graph should reach.
//
//  - no rain at all today      -> next hour (flat line, "no rain")
//  - rain within the next hour -> one hour window (zoomed in)
//  - rain later today          -> stretch the window so the onset sits at
//                                 about two thirds, capped at end of day.
function chooseSpanMinutes(series, now) {
  var endOfDay = new Date(now);
  endOfDay.setHours(23, 59, 0, 0);
  var minutesToEod = Math.round((endOfDay.getTime() - now) / 60000);
  if (minutesToEod < 60) {
    minutesToEod = 60;
  }

  // First future sample at/over the threshold.
  var firstRainMin = -1;
  for (var i = 0; i < series.length; i++) {
    var dtMin = (series[i].t - now) / 60000;
    if (dtMin < 0) {
      continue;
    }
    if (dtMin > minutesToEod) {
      break;
    }
    if (series[i].mmh >= RAIN_THRESHOLD_MMH) {
      firstRainMin = Math.round(dtMin);
      break;
    }
  }

  var span;
  if (firstRainMin < 0) {
    span = 60;
  } else if (firstRainMin <= 60) {
    span = 60;
  } else {
    span = Math.round(firstRainMin * 1.5);
  }
  if (span > minutesToEod) {
    span = minutesToEod;
  }
  if (span < 60) {
    span = 60;
  }
  return { span: span, firstRainMin: firstRainMin };
}

function buildAndSend(series) {
  var now = Date.now();
  var choice = chooseSpanMinutes(series, now);
  var span = choice.span;

  // Sample the chosen window and find the peak.
  var raw = [];
  var peak = 0;
  for (var i = 0; i < NUM_POINTS; i++) {
    var t = now + (span * 60000) * (i / (NUM_POINTS - 1));
    var v = intensityAt(series, t);
    if (v < 0) {
      v = 0;
    }
    if (v > peak) {
      peak = v;
    }
    raw.push(v);
  }

  // Normalise to a scale with a 1 mm/h floor so a light drizzle does not get
  // stretched to fill the whole graph.
  var scaleMax = Math.max(peak, 1.0);
  var points = [];
  for (var j = 0; j < raw.length; j++) {
    var b = Math.round((raw[j] / scaleMax) * 255);
    if (b > 255) {
      b = 255;
    }
    if (b < 0) {
      b = 0;
    }
    points.push(b);
  }

  var msg = {
    "SPAN_MIN": span,
    "PEAK_MMH": Math.round(peak * 100),
    "RAIN_AT_MIN": choice.firstRainMin,
    "NUM_POINTS": NUM_POINTS,
    "POINTS": points
  };
  log("span=" + span + "min peak=" + peak.toFixed(2) + "mm/h rainAt=" +
      choice.firstRainMin + "min");
  Pebble.sendAppMessage(msg, function() {
    log("sent");
  }, function(e) {
    log("send failed: " + JSON.stringify(e));
  });
}

function fetchForecast(lat, lon) {
  var url = "https://api.open-meteo.com/v1/forecast" +
            "?latitude=" + lat.toFixed(4) +
            "&longitude=" + lon.toFixed(4) +
            "&minutely_15=precipitation" +
            "&hourly=precipitation" +
            "&models=knmi_seamless" +
            "&forecast_days=1" +
            "&timezone=auto";
  log("GET " + url);
  var req = new XMLHttpRequest();
  req.open("GET", url, true);
  req.onload = function() {
    try {
      var data = JSON.parse(req.responseText);
      var series = buildTimeline(data);
      if (series.length === 0) {
        sendStatus("No data");
        return;
      }
      buildAndSend(series);
    } catch (err) {
      log("parse error: " + err);
      sendStatus("Bad data");
    }
  };
  req.onerror = function() {
    log("http error");
    sendStatus("No network");
  };
  req.ontimeout = function() {
    sendStatus("Timeout");
  };
  req.timeout = 20000;
  req.send();
}

function update() {
  sendStatus("Locating...");
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      fetchForecast(pos.coords.latitude, pos.coords.longitude);
    },
    function(err) {
      log("geo error " + err.code + ", using default location");
      fetchForecast(DEFAULT_LAT, DEFAULT_LON);
    },
    { timeout: 15000, maximumAge: 600000 }
  );
}

Pebble.addEventListener("ready", function(e) {
  log("ready");
  update();
});

// The watch asks for a refresh on launch and on a timer.
Pebble.addEventListener("appmessage", function(e) {
  log("request from watch");
  update();
});
