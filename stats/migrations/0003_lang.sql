-- The app's language (Settings → Language) and the console's own (System Settings), for deciding
-- which translations to make. Older reports: the app's language from their config.ini.
ALTER TABLE reports ADD COLUMN lang TEXT;      -- "en", "uk", "ja", "zh-Hans", ...
ALTER TABLE reports ADD COLUMN sys_lang TEXT;  -- "ja" "en" "fr" "de" "it" "es" "zh" "ko" "nl" "pt" "ru" "tw"
CREATE INDEX reports_lang ON reports (lang);

UPDATE reports SET lang = COALESCE(
  (SELECT rtrim(substr(rest, 1, instr(rest || char(10), char(10)) - 1), char(13) || ' ')
     FROM (SELECT substr(char(10) || config, instr(char(10) || config, char(10) || 'lang=') + 6) AS rest)
    WHERE instr(char(10) || config, char(10) || 'lang=') > 0),
  'en')
WHERE lang IS NULL AND config IS NOT NULL;
