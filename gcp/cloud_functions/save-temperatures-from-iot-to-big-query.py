from google.cloud import bigquery
import base64, json, sys, os, datetime

OUTDOOR_SENSOR_ID = '9b00000009029f28';

def pubsub_to_bigq(event, context):
    pubsub_message = base64.b64decode(event['data']).decode('utf-8')
    # print(pubsub_message)
    # {sensor_id: a60416586fb5ff28, timestamp: 1676239867, value: 20.5625}
    sensor_data = json.loads(pubsub_message)
    if -100 < float(sensor_data['value']) < 100:
        if OUTDOOR_SENSOR_ID == sensor_data['sensor_id']:
            if datetime.datetime.now().minute % 15 != 0:
                return
        to_bigquery(os.environ['dataset'], os.environ['table'], sensor_data)
    else:
        raise RuntimeError('Value out of range: ' + pubsub_message)

def to_bigquery(dataset, table, document):
    # document['timestamp'] = time.time()
    bigquery_client = bigquery.Client()
    dataset_ref = bigquery_client.dataset(dataset)
    table_ref = dataset_ref.table(table)
    table = bigquery_client.get_table(table_ref)
    errors = bigquery_client.insert_rows(table, [document])
    if errors != [] :
        print(errors, file=sys.stderr)