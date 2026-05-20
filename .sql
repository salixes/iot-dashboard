-- ============================================================
-- TROYIKZ Smart Smoke Detection System
-- Supabase Database Setup Script
-- ============================================================
-- If you get column errors, the table already exists from a
-- previous failed run. This script drops and recreates cleanly.
-- ============================================================

-- Drop existing tables (safe — removes old broken versions)
DROP TABLE IF EXISTS public.smoke_logs CASCADE;
DROP TABLE IF EXISTS public.sensor_status CASCADE;
DROP VIEW  IF EXISTS public.today_summary CASCADE;

-- ============================================================
-- 1. SMOKE LOGS TABLE
-- ============================================================
CREATE TABLE public.smoke_logs (
  id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
  test_id          TEXT        NOT NULL,
  smoke_value      INTEGER     NOT NULL,
  classification   TEXT        NOT NULL,
  status           TEXT        NOT NULL,
  green_led        BOOLEAN     NOT NULL DEFAULT FALSE,
  red_led          BOOLEAN     NOT NULL DEFAULT FALSE,
  buzzer           BOOLEAN     NOT NULL DEFAULT FALSE,
  session_status   TEXT        NOT NULL DEFAULT 'active',
  created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_smoke_logs_created_at     ON public.smoke_logs (created_at DESC);
CREATE INDEX idx_smoke_logs_status         ON public.smoke_logs (status);
CREATE INDEX idx_smoke_logs_session_status ON public.smoke_logs (session_status);
CREATE INDEX idx_smoke_logs_test_id        ON public.smoke_logs (test_id);

-- ============================================================
-- 2. SENSOR STATUS TABLE
-- ============================================================
CREATE TABLE public.sensor_status (
  id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id      TEXT        NOT NULL UNIQUE DEFAULT 'esp32-troyikz-01',
  is_online      BOOLEAN     NOT NULL DEFAULT FALSE,
  is_initialized BOOLEAN     NOT NULL DEFAULT FALSE,
  rssi           INTEGER,
  ip_address     TEXT,
  firmware_ver   TEXT        DEFAULT 'v1.0.0',
  last_seen      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO public.sensor_status (device_id, is_online, is_initialized)
VALUES ('esp32-troyikz-01', FALSE, FALSE)
ON CONFLICT (device_id) DO NOTHING;

-- ============================================================
-- 3. ROW LEVEL SECURITY
-- ============================================================
ALTER TABLE public.smoke_logs    ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.sensor_status ENABLE ROW LEVEL SECURITY;

CREATE POLICY "anon_select_smoke_logs"
  ON public.smoke_logs FOR SELECT TO anon USING (true);

CREATE POLICY "anon_insert_smoke_logs"
  ON public.smoke_logs FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon_update_smoke_logs"
  ON public.smoke_logs FOR UPDATE TO anon USING (true) WITH CHECK (true);

CREATE POLICY "anon_select_sensor_status"
  ON public.sensor_status FOR SELECT TO anon USING (true);

CREATE POLICY "anon_insert_sensor_status"
  ON public.sensor_status FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon_update_sensor_status"
  ON public.sensor_status FOR UPDATE TO anon USING (true) WITH CHECK (true);

-- ============================================================
-- 4. TODAY SUMMARY VIEW
-- ============================================================
CREATE VIEW public.today_summary AS
SELECT
  COUNT(*)                                          AS total,
  COUNT(*) FILTER (WHERE status = 'Safe')          AS safe_count,
  COUNT(*) FILTER (WHERE status = 'Hazardous')     AS hazard_count,
  ROUND(
    COUNT(*) FILTER (WHERE status = 'Safe')::NUMERIC
    / NULLIF(COUNT(*), 0) * 100, 1
  )                                                AS safe_pct
FROM public.smoke_logs
WHERE session_status = 'completed'
  AND created_at >= CURRENT_DATE;

-- ============================================================
-- DONE. Verify with:
--   SELECT column_name FROM information_schema.columns
--   WHERE table_name = 'smoke_logs';
-- ============================================================