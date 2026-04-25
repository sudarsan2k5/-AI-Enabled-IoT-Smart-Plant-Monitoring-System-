"""
FastAPI server for the smart plant project (project.md).
Run from this folder (Windows: open terminal here, then):
  py -3 -m venv .venv
  .venv\\Scripts\\activate
  pip install -r requirements.txt
  uvicorn main:app --host 0.0.0.0 --port 8000
Or:  uvicorn main:app --host 0.0.0.0 --port 8000
"""
from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, ConfigDict, Field

DB_PATH = Path(__file__).resolve().parent / "sensor_data.db"

app = FastAPI(title="Smart Pot Sensor API", version="0.1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


class SensorIn(BaseModel):
    """Body from ESP32 — matches project.md + firmware JSON."""

    model_config = ConfigDict(extra="ignore")

    temperature: int = Field(..., ge=-40, le=60)
    humidity: int = Field(..., ge=0, le=100)
    soil_moisture: int = Field(..., ge=0, le=100)
    light: int = Field(0, ge=0, le=1_000_000)
    nitrogen: int = Field(0, ge=0, le=65_535)
    phosphorus: int = Field(0, ge=0, le=65_535)
    potassium: int = Field(0, ge=0, le=65_535)
    timestamp: str = ""  # from device; if empty we set server time (ISO 8601)


@contextmanager
def get_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
    finally:
        conn.close()


def init_db() -> None:
    with get_db() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS sensor_readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                received_at TEXT NOT NULL,
                device_timestamp TEXT,
                temperature INTEGER,
                humidity INTEGER,
                soil_moisture INTEGER,
                light INTEGER,
                nitrogen INTEGER,
                phosphorus INTEGER,
                potassium INTEGER
            );
            """
        )
        conn.commit()


@app.on_event("startup")
def _startup() -> None:
    init_db()


@app.get("/")
def root() -> dict:
    return {
        "name": "Smart Pot API",
        "docs": "/docs",
        "endpoints": {"POST /sensor-data": "ingest", "GET /data": "latest readings"},
    }


@app.post("/sensor-data", status_code=201)
def post_sensor_data(payload: SensorIn) -> dict:
    now = datetime.now(timezone.utc).replace(microsecond=0)
    dev_ts = payload.timestamp.strip() or now.isoformat().replace("+00:00", "Z")

    with get_db() as conn:
        cur = conn.execute(
            """
            INSERT INTO sensor_readings (
                received_at, device_timestamp, temperature, humidity,
                soil_moisture, light, nitrogen, phosphorus, potassium
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
            """,
            (
                now.isoformat().replace("+00:00", "Z"),
                dev_ts,
                payload.temperature,
                payload.humidity,
                payload.soil_moisture,
                payload.light,
                payload.nitrogen,
                payload.phosphorus,
                payload.potassium,
            ),
        )
        conn.commit()
        rid = cur.lastrowid
    return {"ok": True, "id": rid}


@app.get("/data")
def get_data(limit: int = 200) -> list[dict]:
    if limit < 1 or limit > 2000:
        raise HTTPException(400, "limit must be 1..2000")
    with get_db() as conn:
        rows = conn.execute(
            """
            SELECT
                id,
                received_at,
                device_timestamp,
                temperature,
                humidity,
                soil_moisture,
                light,
                nitrogen,
                phosphorus,
                potassium
            FROM sensor_readings
            ORDER BY id DESC
            LIMIT ?;
            """,
            (limit,),
        ).fetchall()
    return [dict(r) for r in rows]
