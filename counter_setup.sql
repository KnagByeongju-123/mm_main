-- 설비 카운터 Supabase 설정 (생성기 출력) — 재실행 안전

-- 1) 카운트 shot_count  (machine_code = MAC_GPIO)
CREATE TABLE IF NOT EXISTS public.shot_count (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  machine_code varchar(40), shot_count bigint, shot_interval_ms integer,
  detected_at timestamptz DEFAULT now(),
  esp32_mac varchar(20), esp32_ip varchar(20),
  created_at timestamptz DEFAULT now());
ALTER TABLE public.shot_count DISABLE ROW LEVEL SECURITY;
GRANT ALL ON public.shot_count TO anon, authenticated;
CREATE INDEX IF NOT EXISTS idx_shot_count_code ON public.shot_count (machine_code);
CREATE INDEX IF NOT EXISTS idx_shot_count_code_created ON public.shot_count (machine_code, created_at DESC);

-- 2) 기준값(제로세팅) daily_baseline
CREATE TABLE IF NOT EXISTS public.daily_baseline (
  machine_code text, baseline_date date,
  baseline_count bigint DEFAULT 0, created_at timestamptz DEFAULT now(),
  PRIMARY KEY (machine_code, baseline_date));
ALTER TABLE public.daily_baseline DISABLE ROW LEVEL SECURITY;
GRANT ALL ON public.daily_baseline TO anon, authenticated;

NOTIFY pgrst, 'reload schema';
