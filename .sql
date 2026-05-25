-- ============================================================
--  TROYIKZ — Supabase Schema v1.2.0
--  Run entire script in Supabase SQL Editor
--  CHANGES v1.2.0:
--    + commands constraint updated: adds 'S' (Start) and 'X' (Stop)
-- ============================================================

-- ── 1. smoke_logs ──────────────────────────────────────────
CREATE TABLE IF NOT EXISTS public.smoke_logs (
    id              bigserial    PRIMARY KEY,
    test_id         text         NOT NULL,
    smoke_value     integer      NOT NULL CHECK (smoke_value >= 0 AND smoke_value <= 1023),
    classification  text         NOT NULL CHECK (classification IN ('AIR','NORMAL SMOKE','HAZARDOUS SMOKE')),
    status          text         NOT NULL CHECK (status IN ('Safe','Normal','Hazardous')),
    green_led       boolean      NOT NULL DEFAULT false,
    red_led         boolean      NOT NULL DEFAULT false,
    buzzer          boolean      NOT NULL DEFAULT false,
    override        boolean      NOT NULL DEFAULT false,
    session_status  text         NOT NULL DEFAULT 'active'
                                 CHECK (session_status IN ('active','completed')),
    created_at      timestamptz  NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_smoke_logs_created ON public.smoke_logs (created_at DESC);
CREATE INDEX IF NOT EXISTS idx_smoke_logs_test    ON public.smoke_logs (test_id);
CREATE INDEX IF NOT EXISTS idx_smoke_logs_status  ON public.smoke_logs (status);

-- ── 2. sensor_status ───────────────────────────────────────
CREATE TABLE IF NOT EXISTS public.sensor_status (
    id               bigserial   PRIMARY KEY,
    device_id        text        NOT NULL UNIQUE,
    is_online        boolean     NOT NULL DEFAULT false,
    is_initialized   boolean     NOT NULL DEFAULT false,
    warmup_remaining integer     NOT NULL DEFAULT 180 CHECK (warmup_remaining >= 0),
    rssi             integer,
    ip_address       text,
    firmware_ver     text,
    last_seen        timestamptz NOT NULL DEFAULT now()
);
ALTER TABLE public.sensor_status
    DROP CONSTRAINT IF EXISTS sensor_status_device_id_key;
ALTER TABLE public.sensor_status
    ADD CONSTRAINT sensor_status_device_id_key UNIQUE (device_id);

-- ── 3. commands ────────────────────────────────────────────
--  '0'=Force AIR  '1'=Force NORMAL  '2'=Force HAZARDOUS
--  'A'=Auto       'S'=Start Session  'X'=Stop Session
CREATE TABLE IF NOT EXISTS public.commands (
    id          bigserial   PRIMARY KEY,
    command     varchar(2)  NOT NULL,
    executed    boolean     NOT NULL DEFAULT false,
    created_at  timestamptz NOT NULL DEFAULT now()
);

-- Drop old constraint and add updated one with S and X
ALTER TABLE public.commands
    DROP CONSTRAINT IF EXISTS commands_command_check;
ALTER TABLE public.commands
    ADD CONSTRAINT commands_command_check
    CHECK (command IN ('0','1','2','A','S','X'));

CREATE INDEX IF NOT EXISTS idx_commands_pending ON public.commands (executed, created_at ASC);

-- ── Realtime ───────────────────────────────────────────────
DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_publication_tables
    WHERE pubname = 'supabase_realtime' AND tablename = 'smoke_logs'
  ) THEN
    ALTER PUBLICATION supabase_realtime ADD TABLE public.smoke_logs;
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM pg_publication_tables
    WHERE pubname = 'supabase_realtime' AND tablename = 'sensor_status'
  ) THEN
    ALTER PUBLICATION supabase_realtime ADD TABLE public.sensor_status;
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM pg_publication_tables
    WHERE pubname = 'supabase_realtime' AND tablename = 'commands'
  ) THEN
    ALTER PUBLICATION supabase_realtime ADD TABLE public.commands;
  END IF;
END $$;

-- ── RLS ────────────────────────────────────────────────────
ALTER TABLE public.smoke_logs    ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.sensor_status ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.commands      ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "anon_ins_logs" ON public.smoke_logs;
DROP POLICY IF EXISTS "anon_sel_logs" ON public.smoke_logs;
CREATE POLICY "anon_ins_logs" ON public.smoke_logs FOR INSERT TO anon WITH CHECK (true);
CREATE POLICY "anon_sel_logs" ON public.smoke_logs FOR SELECT TO anon USING (true);

DROP POLICY IF EXISTS "anon_ins_ss" ON public.sensor_status;
DROP POLICY IF EXISTS "anon_upd_ss" ON public.sensor_status;
DROP POLICY IF EXISTS "anon_sel_ss" ON public.sensor_status;
CREATE POLICY "anon_ins_ss" ON public.sensor_status FOR INSERT TO anon WITH CHECK (true);
CREATE POLICY "anon_upd_ss" ON public.sensor_status FOR UPDATE TO anon USING (true) WITH CHECK (true);
CREATE POLICY "anon_sel_ss" ON public.sensor_status FOR SELECT TO anon USING (true);

DROP POLICY IF EXISTS "anon_all_cmd" ON public.commands;
CREATE POLICY "anon_all_cmd" ON public.commands FOR ALL TO anon USING (true) WITH CHECK (true);

-- ── Helper views ───────────────────────────────────────────
DROP VIEW IF EXISTS public.v_today_summary;
DROP VIEW IF EXISTS public.v_session_summary;

CREATE VIEW public.v_today_summary AS
SELECT
    COUNT(*)                                              AS total_today,
    COUNT(*) FILTER (WHERE status='Safe')                 AS safe_today,
    COUNT(*) FILTER (WHERE status='Normal')               AS normal_today,
    COUNT(*) FILTER (WHERE status='Hazardous')            AS hazard_today,
    COUNT(DISTINCT test_id)                               AS sessions_today
FROM public.smoke_logs
WHERE created_at >= CURRENT_DATE AT TIME ZONE 'Asia/Manila';

CREATE VIEW public.v_session_summary AS
SELECT
    test_id,
    COUNT(*)                                              AS total_readings,
    MAX(smoke_value)                                      AS peak_ao,
    ROUND(AVG(smoke_value)::numeric, 1)                  AS avg_ao,
    COUNT(*) FILTER (WHERE status='Safe')                 AS safe_count,
    COUNT(*) FILTER (WHERE status='Normal')               AS normal_count,
    COUNT(*) FILTER (WHERE status='Hazardous')            AS hazard_count,
    bool_or(session_status='completed')                   AS is_completed,
    MIN(created_at)                                       AS started_at,
    MAX(created_at)                                       AS ended_at
FROM public.smoke_logs
GROUP BY test_id
ORDER BY MAX(created_at) DESC;