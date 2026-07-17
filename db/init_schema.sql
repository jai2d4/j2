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
    qualifying_tiers      VARCHAR(16)[],            -- every tier cleared, best to worst
    is_game_changer       BOOLEAN NOT NULL DEFAULT FALSE,  -- out-of-bracket flag
    game_changer_reason   TEXT,
    makeup_grades         JSONB,                    -- Module 6: Size/AA/Play History/Play Style/Character input
    makeup_grade_down     JSONB,                    -- overall P4 grade shifted per classification level
    improvement_plan      JSONB,                    -- Module 7: drills/recommendations + per-division target numbers
    model_used            VARCHAR(64) DEFAULT 'gemini-3.5-flash',
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_eval_athlete ON evaluations (athlete_id);
CREATE INDEX idx_eval_game_changer ON evaluations (is_game_changer) WHERE is_game_changer;

-- ------------------------------------------------------------
-- Module 8: web-discovered updates (articles, recruiting-site
-- metric changes) found by a Gemini web search against each
-- saved athlete. Feeds the progression chart alongside evaluations.
-- ------------------------------------------------------------
CREATE TABLE athlete_web_updates (
    id                  UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    athlete_id          UUID NOT NULL REFERENCES athletes(id) ON DELETE CASCADE,
    source_title        TEXT,
    source_url          TEXT,
    published_at        TIMESTAMPTZ,
    summary             TEXT NOT NULL,
    discovered_metrics  JSONB,          -- e.g. {"forty_s": 4.41, "weight_lbs": 195}
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_web_updates_athlete ON athlete_web_updates (athlete_id);

-- ------------------------------------------------------------
-- Module 6: Profile & Makeup grade-down reference (not a table —
-- the shift is computed in app/services/makeup_grade.py). Rank scale,
-- best to worst: GAME_CHANGER, ALL_CONF, WIN_PLUS, WIN, WIN_MINUS, NGE.
-- A P4-standard grade shifts up 1 rank per level going down: Group of 5
-- (+1), FCS (+2), D2/D3/NAIA/JUCO (+3), capped at GAME_CHANGER.
-- ------------------------------------------------------------

-- ============================================================
-- SEED: Position Evaluation Logic Matrix
-- Heights converted: 6'2" = 74.0 in, etc.
-- DL's D1 FCS weight range and D2/D3/NAIA/JUCO tier were interpolated
-- from DE's tier-to-tier step (source page cut off before those numbers).
-- ============================================================
INSERT INTO position_thresholds
(position, tier, height_min_in, height_max_in, weight_min_lbs, weight_max_lbs,
 forty_min_s, forty_max_s, shuttle_max_s, bench_min_lbs, squat_min_lbs,
 gpa_min, sat_min, act_min, trait_notes)
VALUES
-- Offensive skill tiers
('QB','D1_FBS', 74.0, 78.0, 200, 240, 4.60, 4.90, NULL, NULL, NULL, 3.00, 1000, 18,
 'Strong arm (50+ yds), quick release, pre/post-snap reads, pocket awareness'),
('QB','D2_D3_NAIA_JUCO', 72.0, 77.0, 180, 225, 4.70, 5.00, NULL, NULL, NULL, NULL, NULL, NULL,
 'Lower-division QB bracket; decision-making/accuracy upside prioritized'),
('RB','D1_FBS', 69.0, 73.0, 190, 230, 4.40, 4.60, NULL, 300, 450, 3.00, 1000, 18,
 'Explosive through gaps, vision, contact balance, pass-catching'),
('RB','D1_FCS_D2', 68.0, 72.0, 180, 220, 4.50, 4.90, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2/D3/NAIA/JUCO RB bracket'),
('WR','D1_FBS', 72.0, 76.0, 180, 220, 4.30, 4.60, NULL, NULL, NULL, 3.00, 1000, 18,
 'Elite route-running, high-pointing, YAC capability, separation'),
('WR','D1_FCS_D2', 70.0, 75.0, 170, 210, 4.40, 4.90, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2/D3/NAIA/JUCO WR bracket'),
('TE','D1_FBS', 76.0, 79.0, 230, 270, 4.60, 4.80, NULL, 300, 450, 3.00, 1000, 18,
 'In-line blocking mechanics, seam-stretching speed, mismatch generation'),
('TE','D1_FCS_D2', 74.0, 78.0, 220, 260, 4.70, 5.20, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2/D3/NAIA/JUCO TE bracket; development in blocking or receiving'),
('OL','D1_FBS', 76.0, 80.0, 280, 330, 5.00, 5.30, NULL, 350, 500, 3.00, 1000, 18,
 'Hand placement, kick-slide speed, run-blocking power'),
('OL','D1_FCS_D2', 74.0, 78.0, 270, 310, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
 'FCS/D2/D3/NAIA/JUCO OL bracket; physicality/technique over size'),
-- Defensive & trench tiers
('DB','D1_FBS', 70.0, 74.0, 175, 210, 4.40, 4.60, 4.20, NULL, NULL, 3.00, 1000, 18,
 'Hip fluidity, break-on-ball, space amplification; <=4.2s pro-agility target'),
('DB','D1_FCS', 69.0, 73.0, 170, 200, 4.50, 4.70, NULL, NULL, NULL, NULL, NULL, NULL,
 'Zone awareness, recovery speed, open-field tackling'),
('DB','D2_D3_NAIA_JUCO', 68.0, 72.0, 165, 190, 4.60, 4.90, NULL, NULL, NULL, NULL, NULL, NULL,
 'Technique, instincts, versatility to cover multiple positions'),
('LB','D1_FBS', 73.0, 76.0, 220, 250, 4.50, 4.70, NULL, NULL, NULL, 3.00, 1000, 18,
 'Sideline pursuit, block shedding, coverage versatility'),
('LB','D1_FCS', 72.0, 75.0, 210, 240, 4.60, 4.80, NULL, NULL, NULL, NULL, NULL, NULL,
 'Special-teams-ready while developing as a linebacker'),
('LB','D2_D3_NAIA_JUCO', 71.0, 74.0, 200, 230, 4.70, 5.00, NULL, NULL, NULL, NULL, NULL, NULL,
 'Tackling consistency and motor essential'),
('DE','D1_FBS', 75.0, 78.0, 240, 280, 4.60, 4.80, NULL, 350, 500, 3.00, 1000, 18,
 'First-step explosion, bend flexibility, reach extension'),
('DE','D1_FCS', 74.0, 77.0, 230, 270, 4.70, 4.90, NULL, NULL, NULL, NULL, NULL, NULL,
 'Similar traits to FBS but often smaller/less developed'),
('DE','D2_D3_NAIA_JUCO', 72.0, 76.0, 220, 260, 4.80, 5.10, NULL, NULL, NULL, NULL, NULL, NULL,
 'Effort, motor, technique compensate for size/speed'),
('DL','D1_FBS', 75.0, 78.0, 250, 320, 4.80, 5.10, NULL, 350, 500, 3.00, 1000, 18,
 'POA anchor, double-team absorption, interior penetration'),
('DL','D1_FCS', 74.0, 77.0, 240, 300, 4.90, 5.20, NULL, NULL, NULL, NULL, NULL, NULL,
 'Interpolated from DE step-down; source page cut off before this tier'),
('DL','D2_D3_NAIA_JUCO', 72.0, 76.0, 230, 280, 5.00, 5.30, NULL, NULL, NULL, NULL, NULL, NULL,
 'Interpolated from DE step-down; source page cut off before this tier');
