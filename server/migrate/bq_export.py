#!/usr/bin/env python3
"""One-off export of the BigQuery dataset to server/migrate/out/.

Run on the Mac:
  GOOGLE_APPLICATION_CREDENTIALS=gcp/keys/heating-controller-b5fdbb0b2329.json \
    uv run --with google-cloud-bigquery server/migrate/bq_export.py
"""
import csv
import gzip
import json
import pathlib

from google.cloud import bigquery

PROJECT = "heating-controller"
DATASET = "heating_controller_ds"
OUT = pathlib.Path(__file__).resolve().parent / "out"


def main():
    client = bigquery.Client(project=PROJECT)
    (OUT / "views").mkdir(parents=True, exist_ok=True)

    raw = client.get_table(f"{PROJECT}.{DATASET}.temperature_raw")
    count, max_ts = 0, None
    with gzip.open(OUT / "temperature_raw.csv.gz", "wt", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts", "sensor_id", "value"])
        for row in client.list_rows(raw, page_size=100_000):
            w.writerow([row["timestamp"].isoformat(), row["sensor_id"], row["value"]])
            count += 1
            max_ts = row["timestamp"] if max_ts is None else max(max_ts, row["timestamp"])
            if count % 200_000 == 0:
                print(f"  {count} rows")

    sensor = client.get_table(f"{PROJECT}.{DATASET}.sensor")
    fields = [f.name for f in sensor.schema]
    with open(OUT / "sensor.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(fields)
        for row in client.list_rows(sensor):
            w.writerow([row[k] for k in fields])

    for item in client.list_tables(DATASET):
        table = client.get_table(item)
        if table.table_type == "VIEW":
            (OUT / "views" / f"{item.table_id}.sql").write_text(table.view_query + "\n")

    summary = {"rows": count, "table_num_rows": raw.num_rows, "max_ts": max_ts.isoformat() if max_ts else None}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
