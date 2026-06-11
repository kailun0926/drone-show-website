-- =====================================================
--  MySQL schema for the mesh positioning backend
--
--  Run once to set up the database:
--    mysql -u root -p < schema.sql
-- =====================================================

CREATE DATABASE IF NOT EXISTS mesh_nodes
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE mesh_nodes;

-- Stitched positions — the aggregator's output, one row per node per solve.
CREATE TABLE IF NOT EXISTS node_position (
  id        BIGINT AUTO_INCREMENT PRIMARY KEY,
  ts        DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  node_id   VARCHAR(8)  NOT NULL,        -- e.g. "0xAB"
  is_anchor BOOLEAN     NOT NULL,
  x         DOUBLE,
  y         DOUBLE,
  altitude  DOUBLE,
  azimuth   INT,
  INDEX idx_ts (ts),
  INDEX idx_node_ts (node_id, ts)
);

-- Raw per-node sensor packets (used by the simulator; handy for richer
-- dashboards). The hardware path currently only streams positions, but this
-- table is here so the backend and simulator agree.
CREATE TABLE IF NOT EXISTS sensor_data (
  id         BIGINT AUTO_INCREMENT PRIMARY KEY,
  ts         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  node_id    VARCHAR(8)  NOT NULL,
  is_anchor  BOOLEAN     NOT NULL,
  temp       DOUBLE,
  pressure   INT,
  altitude   DOUBLE,
  azimuth    INT,
  ax DOUBLE, ay DOUBLE, az DOUBLE,       -- accelerometer
  gx DOUBLE, gy DOUBLE, gz DOUBLE,       -- gyroscope
  cx DOUBLE, cy DOUBLE, cz DOUBLE,       -- magnetometer
  peer_count INT,
  peers_json JSON,                       -- [{"id":"0xCD","rssi":-55}, ...]
  INDEX idx_ts (ts),
  INDEX idx_node_ts (node_id, ts)
);
