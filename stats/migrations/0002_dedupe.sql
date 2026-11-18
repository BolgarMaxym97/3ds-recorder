-- A report whose 2xx got lost is sent again from the same marks: it replaces the row it repeats
-- (with the lines added since), and a dump sent twice is kept once.
ALTER TABLE reports ADD COLUMN sent_from TEXT;  -- the app's marks before this report ("app/rec" sig:len:off)
CREATE UNIQUE INDEX reports_sent_from ON reports (install_id, sent_from);
CREATE UNIQUE INDEX dumps_name ON dumps (install_id, name);
