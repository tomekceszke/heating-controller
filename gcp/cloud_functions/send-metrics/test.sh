 curl -i -X POST https://europe-west3-water-controller-351109.cloudfunctions.net/send-metrics \
 -H "Authorization: bearer $(gcloud auth print-identity-token)" \
 -H "Content-Type: application/json" \
 -d '{"start_time":"1691791039","stop_time":"1691791040","consumption":"0"}'
