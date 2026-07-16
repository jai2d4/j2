-- ============================================================
-- TRU SCOUTING ENGINE — PostgreSQL Initialization Schema
-- Module 5: Relational Database Matrix
-- ============================================================

CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- ------------------------------------------------------------
-- Position threshold matrix (the hard-coded eval logic tables)
-- Heights stored in inches, speeds in seconds for range math.
-- ------------------------------------------------------------
CREATE TABLE position_thresholds (
    id              SERIAL PRIMARY KEY,
    position        VARCHAR(4)  NOT NULL,          -- QB, RB, WR, DB, LB, DE, DL, OL, TE
    tier            VARCHAR(16) NOT NULL,          -- D1_FBS, D1_FCS, D2, D3, NAIA, JUCO
    height_min_in   NUMERIC(4,1),
    height_max_in   NUMERIC(4,1),
    weight_min_lbs  INT,
    weight_max_lbs  INT,
    forty_min_s     NUMERIC(3,2),
    forty_max_s     NUMERIC(3,2),
    shuttle_max_s   NUMERIC(3,2),                  -- pro-agility ceiling (laser metric)
    bench_min_lbs   INT,
    squat_min_lbs   INT,
    gpa_min         NUMERIC(3,2),
    sat_min         INT,
    act_min         INT,
    trait_notes     TEXT,
    UNIQUE (position, tier)
);

-- ------------------------------------------------------------
-- Athletes
-- ------------------------------------------------------------
CREATE TABLE athletes (
    id              UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    first_name      VARCHAR(64) NOT NULL,
    last_name       VARCHAR(64) NOT NULL,
    grad_year       INT,
    school          VARCHAR(128),
    state           VARCHAR(2),
    position        VARCHAR(4)  NOT NULL,
    height_in       NUMERIC(4,1),
    weight_lbs      INT,
    forty_s         NUMERIC(3,2),                  -- laser-timed 40
    shuttle_s       NUMERIC(3,2),                  -- laser-timed pro-agility
    bench_lbs       INT,
    squat_lbs       INT,
    gpa             NUMERIC(3,2),
    sat             INT,
    act             INT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_athletes_position ON athletes (position);
CREATE INDEX idx_athletes_grad_year ON athletes (grad_year);

-- ------------------------------------------------------------
-- Film uploads (Module 1)
-- ------------------------------------------------------------
CREATE TABLE film_uploads (
    id              UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    athlete_id      UUID REFERENCES athletes(id) ON DELETE CASCADE,
    filename        VARCHAR(255) NOT NULL,
    gemini_file_id  VARCHAR(255),                  -- ai_client.files.upload() handle
    mime_type       VARCHAR(32),
    duration_s      NUMERIC(8,2),
    status          VARCHAR(16) NOT NULL DEFAULT 'pending'
                    CHECK (status IN ('pending','processing','analyzed','failed')),
    uploaded_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_film_athlete ON film_uploads (athlete_id);

-- ------------------------------------------------------------
-- Evaluations / Truth Reports (Modules 2–4)
-- ------------------------------------------------------------
CREATE TABLE evaluations (
    id                    UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    athlete_id            UUID NOT NULL REFERENCES athletes(id) ON DELETE CASCADE,
    film_id               UUID REFERENCES film_uploads(id) ON DELETE SET NULL,
    position_evaluated    VARCHAR(4) NOT NULL,
    projected_tier        VARCHAR(16),             -- highest tier where all hard metrics pass
    physical_projection   JSONB,                   -- raw Gemini structured output
    explosive_traits      JSONB,
    mechanics_grade       NUMERIC(3,1) CHECK (mechanics_grade BETWEEN 0 AND 10),
    situational_attributes JSONB,
    metric_sieve_results  JSONB,                   -- per-threshold pass/fail detail
    is_game_changer       BOOLEAN NOT NULL DEFAULT FALSE,  -- out-of-bracket flag
    game_changer_reason   TEXT,
    model_used            VARCHAR(64) DEFAULT 'gemini-3.5-flash',
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_eval_athlete ON evaluations (athlete_id);
CREATE INDEX idx_eval_game_changer ON evaluations (is_game_changer) WHERE is_game_changer;

-- ============================================================
-- SEED: Position Evaluation Logic Matrix (exact blueprint values)
-- Heights converted: 6'2" = 74.0 in, etc.
-- ============================================================
INSERT INTO position_thresholds
(position, tier, height_min_in, height_max_in, weight_min_lbs, weight_max_lbs,
 forty_min_s, forty_max_s, shuttle_max_s, bench_min_lbs, squat_min_lbs,
 gpa_min, sat_min, act_min, trait_notes)
VALUES
-- Offensive skill tiers
('QB','D1_FBS', 74.0, 78.0, 200, 240, 4.60, 4.90, NULL, NULL, NULL, 3.00, 1000, 18,
 'Strong arm (50+ yds), quick release, pre/post-snap reads, pocket awareness'),
('QB','D1_FCS', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
 'Size varied; developmental focus with upside over elite frame'),
('QB','D2_D3_NAIA_JUCO', 72.0, 77.0, 180, 225, 4.70, 5.00, NULL, NULL, NULL, NULL, NULL, NULL,
 'Lower-division QB bracket'),
('RB','D1_FBS', 69.0, 73.0, 190, 230, 4.40, 4.60, NULL, 300, 450, 3.00, 1000, 18,
 'Explosive through gaps, vision, contact balance, pass-catching'),
('RB','D1_FCS_D2', 68.0, 72.0, 180, 220, 4.50, 4.70, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2 RB bracket'),
('WR','D1_FBS', 72.0, 76.0, 180, 220, 4.30, 4.60, NULL, NULL, NULL, 3.00, 1000, 18,
 'Elite route-running, high-pointing, YAC capability, separation'),
('WR','D1_FCS_D2', 70.0, 75.0, 170, 210, 4.40, 4.70, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2 WR bracket'),
-- Defensive & trench tiers
('DB','D1_FBS', 70.0, 74.0, 175, 210, 4.40, 4.60, 4.20, NULL, NULL, NULL, NULL, NULL,
 'Hip fluidity, break-on-ball, space amplification; <=4.2s pro-agility target'),
('DB','D1_FCS', 69.0, 73.0, 170, 200, 4.50, 4.70, NULL, NULL, NULL, NULL, NULL, NULL,
 'Zone awareness, recovery speed, open-field tackling'),
('LB','D1_FBS', 73.0, 76.0, 220, 250, 4.50, 4.70, NULL, NULL, NULL, NULL, NULL, NULL,
 'Sideline pursuit, block shedding, coverage versatility'),
('DE','D1_FBS', 75.0, 78.0, 240, 280, 4.60, 4.80, NULL, NULL, NULL, NULL, NULL, NULL,
 'First-step explosion, bend flexibility, reach extension'),
('DL','D1_FBS', 75.0, 78.0, 250, 320, 4.80, 5.10, NULL, NULL, NULL, NULL, NULL, NULL,
 'POA anchor, double-team absorption, interior penetration'),
('OL','D1_FBS', 76.0, 80.0, 280, 330, 5.00, 5.30, NULL, NULL, NULL, NULL, NULL, NULL,
 'Hand placement, kick-slide speed, run-blocking power'),
('TE','D1_FBS', 76.0, 79.0, 230, 270, 4.60, 4.80, NULL, NULL, NULL, NULL, NULL, NULL,
 'In-line blocking mechanics, seam-stretching speed, mismatch generation');
