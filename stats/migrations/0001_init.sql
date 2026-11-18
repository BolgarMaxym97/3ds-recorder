-- One row per app launch that had something to send. No IP, no country: the Worker never reads them.
CREATE TABLE reports (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  received_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
  install_id     TEXT    NOT NULL,  -- random, made on the console; shown in Settings
  app_version    TEXT    NOT NULL,  -- "1.5.0"
  channel        TEXT,              -- "" | "beta"
  build          TEXT,              -- "cia" | "3dsx"
  model          TEXT,              -- "New 3DS XL", ...
  region         TEXT,              -- "EUR", ...
  system_version TEXT,              -- "11.17.0-50E"
  luma           TEXT,              -- "13.5.0"
  kernel         TEXT,              -- "2.58-0"
  config         TEXT,              -- config.ini as it is on the SD card
  -- Read from rec_log by the Worker, so they stay when the log text is dropped
  titles         TEXT,              -- distinct title ids from "program started", comma-separated
  starts         INTEGER NOT NULL DEFAULT 0,  -- "recorder started" lines
  recordings     INTEGER NOT NULL DEFAULT 0,  -- "recording saved" lines
  dropped        INTEGER NOT NULL DEFAULT 0,  -- sum of "dropped N" in them
  hangs          INTEGER NOT NULL DEFAULT 0,  -- "system notification 203": POWER held, a forced power-off
  app_log        TEXT,              -- new lines of app.log since the last report (NULL after 180 days)
  rec_log        TEXT               -- new lines of recorder.log since the last report (NULL after 180 days)
);

CREATE INDEX reports_install ON reports (install_id, received_at);
CREATE INDEX reports_received ON reports (received_at);
CREATE INDEX reports_model ON reports (model);

-- Luma ARM11 crash dumps (sdmc:/luma/dumps/arm11/crash_dump_NNNNNNNN.dmp); deleted after 180 days.
CREATE TABLE dumps (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  received_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
  install_id   TEXT    NOT NULL,
  name         TEXT    NOT NULL,
  size         INTEGER NOT NULL,
  data         BLOB    NOT NULL
);

CREATE INDEX dumps_install ON dumps (install_id, received_at);
CREATE INDEX dumps_received ON dumps (received_at);
